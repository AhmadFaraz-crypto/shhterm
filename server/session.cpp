#include "session.hpp"

#include <algorithm>
#include <utility>

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

namespace shhterm::server {

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using proto::json;

namespace {

// Rate limiter: burst of 10 messages, refilling at 5 messages/second.
constexpr double kBucketCapacity = 10.0;
constexpr double kRefillPerSecond = 5.0;

// Chunk size for socket reads.
constexpr std::size_t kReadChunk = 8 * 1024;

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

Session::Session(ssl_stream stream, RoomManager& rooms)
    : stream_(std::move(stream)),
      rooms_(rooms),
      tokens_(kBucketCapacity),
      last_refill_(std::chrono::steady_clock::now()) {
  read_buf_.resize(kReadChunk);
}

void Session::start() {
  auto self = shared_from_this();
  stream_.async_handshake(ssl::stream_base::server,
                          [this, self](const boost::system::error_code& ec) {
                            if (ec) {
                              close();
                              return;
                            }
                            do_read();
                          });
}

void Session::do_read() {
  auto self = shared_from_this();
  stream_.async_read_some(
      asio::buffer(read_buf_),
      [this, self](const boost::system::error_code& ec, std::size_t n) {
        if (ec) {
          close();
          return;
        }
        if (!parser_.feed(read_buf_.data(), n, pending_frames_)) {
          send_error("too_large", "frame exceeded size limit");
          close();
          return;
        }
        on_frames();
        if (!closed_) do_read();
      });
}

void Session::on_frames() {
  std::vector<std::string> frames;
  frames.swap(pending_frames_);
  for (const auto& payload : frames) {
    auto msg = proto::parse_message(payload);
    if (!msg) {
      send_error("bad_request", "malformed message");
      continue;
    }
    handle(*msg);
    if (closed_) return;
  }
}

void Session::handle(const json& msg) {
  const std::string type = msg.value("t", "");
  if (type == "create") {
    handle_create(msg);
  } else if (type == "join") {
    handle_join(msg);
  } else if (type == "msg") {
    handle_msg(msg);
  } else if (type == "nick") {
    handle_nick(msg);
  } else if (type == "typing") {
    handle_typing();
  } else if (type == "users") {
    handle_users();
  } else if (type == "leave") {
    leave_room(/*graceful=*/true);
  } else if (type == "ping") {
    send({{"t", "pong"}});
  } else {
    send_error("bad_request", "unknown message type");
  }
}

void Session::handle_create(const json& msg) {
  if (room_) {
    send_error("bad_request", "already in a room");
    return;
  }
  if (msg.value("ver", 0) != proto::kProtocolVersion) {
    send_error("bad_version", "unsupported protocol version");
    return;
  }
  std::string name = msg.value("name", "");
  if (!proto::valid_name(name)) {
    send_error("bad_request", "invalid name");
    return;
  }
  auto room = rooms_.create();
  name_ = name;
  room->add(shared_from_this());
  room_ = room;
  rooms_.note_join(room);
  send({{"t", "welcome"},
        {"key", room->display_key()},
        {"name", name_},
        {"users", room->names()}});
}

void Session::handle_join(const json& msg) {
  if (room_) {
    send_error("bad_request", "already in a room");
    return;
  }
  if (msg.value("ver", 0) != proto::kProtocolVersion) {
    send_error("bad_version", "unsupported protocol version");
    return;
  }
  std::string name = msg.value("name", "");
  if (!proto::valid_name(name)) {
    send_error("bad_request", "invalid name");
    return;
  }
  std::string norm = proto::normalize_key(msg.value("key", ""));
  auto room = norm.empty() ? nullptr : rooms_.find(norm);
  if (!room) {
    send_error("invalid_key", "no room with that key");
    return;
  }
  if (room->has(name)) {
    send_error("duplicate_name", "that name is taken in this room");
    return;
  }
  name_ = name;
  room->add(shared_from_this());
  room_ = room;
  rooms_.note_join(room);
  // Tell the newcomer who is here (including themselves).
  send({{"t", "welcome"},
        {"key", room->display_key()},
        {"name", name_},
        {"users", room->names()}});
  // Notify everyone else.
  room->broadcast(
      proto::encode({{"t", "presence"}, {"event", "join"}, {"name", name_}}),
      name_);
}

void Session::handle_msg(const json& msg) {
  if (!room_) {
    send_error("not_in_room", "join a room first");
    return;
  }
  if (!msg.contains("id") || !msg["id"].is_number_integer()) {
    send_error("bad_request", "msg requires integer id");
    return;
  }
  // The body is end-to-end encrypted; the server treats it as an opaque string
  // and never attempts to read it.
  if (!msg.contains("enc") || !msg["enc"].is_string()) {
    send_error("bad_request", "msg requires enc");
    return;
  }
  const std::string enc = msg["enc"].get<std::string>();
  // kMaxTextBytes is the plaintext bound; allow headroom for base64 + nonce+tag.
  if (enc.size() > proto::kMaxTextBytes * 2 + 64) {
    send_error("too_large", "message too long");
    return;
  }
  if (!allow_message()) {
    send_error("rate_limited", "slow down");
    return;  // deliberately no ack: the message was not routed
  }

  // Optional direct message to a single recipient.
  if (msg.contains("to") && msg["to"].is_string()) {
    const std::string to = msg["to"].get<std::string>();
    bool ok = room_->deliver_to(
        to, proto::encode({{"t", "msg"},
                           {"from", name_},
                           {"enc", enc},
                           {"ts", now_ms()},
                           {"dm", true}}));
    if (!ok) {
      send_error("no_such_user", "no user '" + to + "' in this room");
      return;  // no ack: not routed
    }
  } else {
    // Route to everyone else.
    room_->broadcast(proto::encode({{"t", "msg"},
                                    {"from", name_},
                                    {"enc", enc},
                                    {"ts", now_ms()}}),
                     name_);
  }
  // Ack means "accepted and routed by the server" — not that a peer has read it.
  send({{"t", "ack"}, {"id", msg["id"]}});
}

void Session::handle_nick(const json& msg) {
  if (!room_) {
    send_error("not_in_room", "join a room first");
    return;
  }
  std::string want = msg.value("name", "");
  if (!proto::valid_name(want)) {
    send_error("bad_request", "invalid name");
    return;
  }
  if (want == name_) return;  // no-op
  if (!room_->rename(name_, want)) {
    send_error("duplicate_name", "that name is taken in this room");
    return;
  }
  std::string old = name_;
  name_ = want;
  // Tell everyone (including the renamer, so their own UI can confirm).
  room_->broadcast(
      proto::encode({{"t", "nick"}, {"old", old}, {"name", want}}));
}

void Session::handle_typing() {
  if (!room_) return;  // silently ignore before joining
  // Throttle to at most one broadcast per second so keystroke spam can't flood
  // the room. This is separate from the chat-message rate budget.
  auto now = std::chrono::steady_clock::now();
  if (now - last_typing_ < std::chrono::seconds(1)) return;
  last_typing_ = now;
  room_->broadcast(proto::encode({{"t", "typing"}, {"from", name_}}), name_);
}

void Session::handle_users() {
  if (!room_) {
    send_error("not_in_room", "join a room first");
    return;
  }
  send({{"t", "users"}, {"users", room_->names()}});
}

void Session::leave_room(bool graceful) {
  if (!room_) return;
  auto room = room_;
  room_.reset();
  room->remove(name_);
  room->broadcast(
      proto::encode({{"t", "presence"}, {"event", "leave"}, {"name", name_}}));
  rooms_.note_leave(room);
  if (graceful) {
    // A graceful leave is also a disconnect from the client's perspective.
    close();
  }
}

void Session::deliver(const std::string& frame) {
  if (closed_) return;
  write_queue_.push_back(frame);
  if (!writing_) do_write();
}

void Session::send(const json& msg) { deliver(proto::encode(msg)); }

void Session::send_error(const std::string& code, const std::string& message) {
  send({{"t", "error"}, {"code", code}, {"message", message}});
}

void Session::do_write() {
  if (write_queue_.empty()) {
    writing_ = false;
    return;
  }
  writing_ = true;
  auto self = shared_from_this();
  asio::async_write(
      stream_, asio::buffer(write_queue_.front()),
      [this, self](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
          close();
          return;
        }
        write_queue_.pop_front();
        do_write();
      });
}

void Session::close() {
  if (closed_) return;
  closed_ = true;
  // Detach from the room and notify peers (non-graceful path).
  if (room_) {
    auto room = room_;
    room_.reset();
    room->remove(name_);
    room->broadcast(proto::encode(
        {{"t", "presence"}, {"event", "leave"}, {"name", name_}}));
    rooms_.note_leave(room);
  }
  boost::system::error_code ec;
  stream_.lowest_layer().cancel(ec);
  // Best-effort TLS shutdown; ignore errors on a dying connection.
  stream_.async_shutdown([self = shared_from_this()](
                             const boost::system::error_code&) {});
}

bool Session::allow_message() {
  auto now = std::chrono::steady_clock::now();
  double elapsed =
      std::chrono::duration<double>(now - last_refill_).count();
  last_refill_ = now;
  tokens_ = std::min(kBucketCapacity, tokens_ + elapsed * kRefillPerSecond);
  if (tokens_ < 1.0) return false;
  tokens_ -= 1.0;
  return true;
}

}  // namespace shhterm::server
