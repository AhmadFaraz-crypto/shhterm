#include "net_client.hpp"

#include <iostream>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/write.hpp>

namespace shhterm::client {

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using asio::ip::tcp;
using proto::json;

namespace {
constexpr std::size_t kReadChunk = 8 * 1024;
constexpr std::chrono::milliseconds kMaxBackoff{30000};
}  // namespace

const char* to_string(ConnState s) {
  switch (s) {
    case ConnState::Connecting: return "connecting";
    case ConnState::Connected: return "connected";
    case ConnState::Reconnecting: return "reconnecting";
    case ConnState::Failed: return "failed";
    case ConnState::Closed: return "closed";
  }
  return "?";
}

NetClient::NetClient(ClientConfig cfg, std::function<void(Event)> on_event)
    : cfg_(std::move(cfg)),
      on_event_(std::move(on_event)),
      ssl_ctx_(ssl::context::tls_client),
      resolver_(io_),
      backoff_timer_(io_),
      work_guard_(asio::make_work_guard(io_)) {
  read_buf_.resize(kReadChunk);

  ssl_ctx_.set_options(ssl::context::default_workarounds |
                       ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                       ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);

  if (cfg_.insecure_skip_verify) {
    ssl_ctx_.set_verify_mode(ssl::verify_none);
  } else {
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    if (!cfg_.ca_file.empty()) {
      ssl_ctx_.load_verify_file(cfg_.ca_file);
    } else {
      ssl_ctx_.set_default_verify_paths();
    }
  }

  // Establish the end-to-end encryption secret. On create we generate a fresh
  // secret half; on join we extract it from the supplied full key. Either way
  // the secret never travels to the server.
  if (cfg_.create) {
    secret_ = proto::generate_secret();
  } else if (auto halves = proto::split_room_key(cfg_.key)) {
    secret_ = halves->second;
    cfg_.key = halves->first;  // send only the routing half to the server
  }
  if (!secret_.empty())
    cipher_ = std::make_unique<crypto::MessageCipher>(secret_);
}

NetClient::~NetClient() { stop(); }

void NetClient::start() {
  io_thread_ = std::thread([this] {
    connect();
    io_.run();
  });
}

void NetClient::stop() {
  if (shutdown_.exchange(true)) return;  // run teardown (and join) exactly once
  stopping_.store(true);                 // also suppress any reconnection
  // Tear down from within the io thread for clean shutdown.
  asio::post(io_, [this] {
    boost::system::error_code ec;
    backoff_timer_.cancel();
    if (stream_) stream_->lowest_layer().close(ec);
    work_guard_.reset();
  });
  if (io_thread_.joinable()) io_thread_.join();
}

void NetClient::emit(Event ev) {
  if (on_event_) on_event_(std::move(ev));
}

void NetClient::set_state(ConnState s, const std::string& text) {
  state_.store(s);
  Event ev;
  ev.type = Event::Type::Status;
  ev.state = s;
  ev.text = text;
  emit(std::move(ev));
}

void NetClient::connect() {
  if (stopping_) return;
  stream_ = std::make_unique<ssl::stream<tcp::socket>>(io_, ssl_ctx_);

  // Set SNI so the server can present the right cert.
  const std::string& sni =
      cfg_.verify_host.empty() ? cfg_.host : cfg_.verify_host;
  SSL_set_tlsext_host_name(stream_->native_handle(), sni.c_str());
  if (!cfg_.insecure_skip_verify) {
    stream_->set_verify_callback(ssl::host_name_verification(sni));
  }

  set_state(joined_once_ ? ConnState::Reconnecting : ConnState::Connecting,
            joined_once_ ? "reconnecting to " + cfg_.host + ":" + cfg_.port
                         : "connecting to " + cfg_.host + ":" + cfg_.port);

  resolver_.async_resolve(
      cfg_.host, cfg_.port,
      [this](const boost::system::error_code& ec,
             tcp::resolver::results_type results) { on_resolved(ec, results); });
}

void NetClient::on_resolved(const boost::system::error_code& ec,
                            tcp::resolver::results_type results) {
  if (ec) {
    handle_disconnect("cannot resolve host: " + ec.message());
    return;
  }
  asio::async_connect(
      stream_->lowest_layer(), results,
      [this](const boost::system::error_code& ec, const tcp::endpoint&) {
        on_connected(ec);
      });
}

void NetClient::on_connected(const boost::system::error_code& ec) {
  if (ec) {
    handle_disconnect("cannot connect: " + ec.message());
    return;
  }
  stream_->async_handshake(
      ssl::stream_base::client,
      [this](const boost::system::error_code& ec) { on_handshake(ec); });
}

void NetClient::on_handshake(const boost::system::error_code& ec) {
  if (ec) {
    handle_disconnect("TLS handshake failed: " + ec.message());
    return;
  }
  send_hello();
  do_read();
}

void NetClient::send_hello() {
  json hello;
  if (cfg_.create) {
    hello = {{"t", "create"}, {"name", cfg_.name}, {"ver", proto::kProtocolVersion}};
  } else {
    hello = {{"t", "join"},
             {"key", cfg_.key},
             {"name", cfg_.name},
             {"ver", proto::kProtocolVersion}};
  }
  enqueue(proto::encode(hello));
}

void NetClient::do_read() {
  stream_->async_read_some(
      asio::buffer(read_buf_),
      [this](const boost::system::error_code& ec, std::size_t n) {
        if (ec) {
          handle_disconnect("connection lost: " + ec.message());
          return;
        }
        if (!parser_.feed(read_buf_.data(), n, pending_frames_)) {
          handle_disconnect("protocol error: " + parser_.error());
          return;
        }
        std::vector<std::string> frames;
        frames.swap(pending_frames_);
        for (auto& payload : frames) on_payload(payload);
        do_read();
      });
}

void NetClient::on_payload(const std::string& payload) {
  auto msg = proto::parse_message(payload);
  if (!msg) return;
  const std::string type = msg->value("t", "");

  if (type == "welcome") {
    joined_once_ = true;
    backoff_ = std::chrono::seconds(1);  // reset backoff on success
    state_.store(ConnState::Connected);
    Event ev;
    ev.type = Event::Type::Welcome;
    ev.state = ConnState::Connected;
    // The server returns the routing half; combine with our secret to form the
    // full shareable key the user will distribute.
    ev.key = proto::compose_room_key(msg->value("key", ""), secret_);
    ev.from = msg->value("name", "");
    if (msg->contains("users") && (*msg)["users"].is_array())
      ev.users = (*msg)["users"].get<std::vector<std::string>>();
    emit(std::move(ev));
  } else if (type == "msg") {
    Event ev;
    ev.type = Event::Type::Chat;
    ev.from = msg->value("from", "");
    ev.dm = msg->value("dm", false);
    // Decrypt the end-to-end-encrypted body. A failure means a wrong/missing
    // key or tampering; surface it rather than showing garbage.
    std::string enc = msg->value("enc", "");
    std::optional<std::string> plain;
    if (cipher_) plain = cipher_->decrypt(enc);
    ev.text = plain ? *plain : std::string("[unable to decrypt message]");
    emit(std::move(ev));
  } else if (type == "nick") {
    Event ev;
    ev.type = Event::Type::Nick;
    ev.from = msg->value("old", "");
    ev.text = msg->value("name", "");
    emit(std::move(ev));
  } else if (type == "typing") {
    Event ev;
    ev.type = Event::Type::Typing;
    ev.from = msg->value("from", "");
    emit(std::move(ev));
  } else if (type == "ack") {
    Event ev;
    ev.type = Event::Type::Ack;
    ev.id = msg->value("id", 0LL);
    emit(std::move(ev));
  } else if (type == "presence") {
    Event ev;
    ev.type = Event::Type::Presence;
    ev.from = msg->value("name", "");
    const std::string event = msg->value("event", "");
    ev.text = ev.from + (event == "join" ? " joined" : " left");
    emit(std::move(ev));
  } else if (type == "users") {
    Event ev;
    ev.type = Event::Type::Users;
    if (msg->contains("users") && (*msg)["users"].is_array())
      ev.users = (*msg)["users"].get<std::vector<std::string>>();
    emit(std::move(ev));
  } else if (type == "error") {
    Event ev;
    ev.type = Event::Type::Notice;
    ev.text = "server error [" + msg->value("code", "?") +
              "]: " + msg->value("message", "");
    emit(std::move(ev));
    // A fatal join error (bad key / taken name) should not spin in reconnect.
    const std::string code = msg->value("code", "");
    if (!joined_once_ &&
        (code == "invalid_key" || code == "duplicate_name" ||
         code == "bad_version")) {
      stopping_.store(true);
      set_state(ConnState::Failed, "cannot join: " + code);
      boost::system::error_code ec2;
      if (stream_) stream_->lowest_layer().close(ec2);
    }
  }
  // "pong" is ignored.
}

void NetClient::enqueue(std::string frame) {
  write_queue_.push_back(std::move(frame));
  if (!writing_) do_write();
}

void NetClient::do_write() {
  if (write_queue_.empty()) {
    writing_ = false;
    return;
  }
  writing_ = true;
  asio::async_write(
      *stream_, asio::buffer(write_queue_.front()),
      [this](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
          handle_disconnect("write failed: " + ec.message());
          return;
        }
        write_queue_.pop_front();
        do_write();
      });
}

void NetClient::handle_disconnect(const std::string& why) {
  writing_ = false;
  write_queue_.clear();
  parser_ = proto::FrameParser{};
  pending_frames_.clear();
  boost::system::error_code ec;
  if (stream_) stream_->lowest_layer().close(ec);

  if (stopping_) {
    set_state(ConnState::Closed, "disconnected");
    return;
  }
  {
    Event ev;
    ev.type = Event::Type::Notice;
    ev.text = why;
    emit(std::move(ev));
  }
  schedule_reconnect();
}

void NetClient::schedule_reconnect() {
  if (stopping_) return;
  auto delay = backoff_;
  set_state(ConnState::Reconnecting,
            "reconnecting in " +
                std::to_string(
                    std::chrono::duration_cast<std::chrono::seconds>(delay)
                        .count()) +
                "s");
  backoff_ = std::min(kMaxBackoff, backoff_ * 2);
  backoff_timer_.expires_after(delay);
  backoff_timer_.async_wait([this](const boost::system::error_code& ec) {
    if (ec || stopping_) return;
    connect();
  });
}

long long NetClient::send_chat(const std::string& text, const std::string& to) {
  if (state_.load() != ConnState::Connected || !cipher_) return 0;
  // Encrypt before handing to the io thread so plaintext never enters a queue.
  std::string enc;
  try {
    enc = cipher_->encrypt(text);
  } catch (...) {
    return 0;
  }
  long long id = next_id_.fetch_add(1);
  asio::post(io_, [this, id, enc, to] {
    if (state_.load() != ConnState::Connected) return;
    proto::json m{{"t", "msg"}, {"id", id}, {"enc", enc}};
    if (!to.empty()) m["to"] = to;
    enqueue(proto::encode(m));
  });
  return id;
}

void NetClient::send_nick(const std::string& new_name) {
  asio::post(io_, [this, new_name] {
    if (state_.load() != ConnState::Connected) return;
    enqueue(proto::encode({{"t", "nick"}, {"name", new_name}}));
  });
}

void NetClient::send_typing() {
  asio::post(io_, [this] {
    if (state_.load() != ConnState::Connected) return;
    enqueue(proto::encode({{"t", "typing"}}));
  });
}

void NetClient::request_users() {
  asio::post(io_, [this] {
    if (state_.load() != ConnState::Connected) return;
    enqueue(proto::encode({{"t", "users"}}));
  });
}

}  // namespace shhterm::client
