#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>

#include "crypto.hpp"
#include "protocol.hpp"

namespace shhterm::client {

enum class ConnState { Connecting, Connected, Reconnecting, Failed, Closed };

const char* to_string(ConnState s);

// An event delivered from the network thread to the UI. The UI is responsible
// for thread-safe handling (it owns the callback).
struct Event {
  enum class Type {
    Status,     // connection state changed (see state, text)
    Welcome,    // joined/created a room (key, name, users populated)
    Chat,       // a peer message (from, text decrypted; dm set for direct msgs)
    Presence,   // join/leave (text already formatted, from = name)
    Nick,       // someone renamed (from = old name, text = new name)
    Typing,     // a peer is typing (from = name)
    Users,      // reply to /users (users populated)
    Ack,        // server acknowledged our message id
    Notice,     // local/server informational or error text
  };
  Type type;
  ConnState state = ConnState::Connecting;
  std::string text;
  std::string from;
  std::string key;   // full shareable key (routing + secret) on Welcome
  std::vector<std::string> users;
  long long id = 0;
  bool dm = false;   // Chat: true if this was a direct message
};

struct ClientConfig {
  std::string host;
  std::string port;
  std::string name;
  std::string key;             // empty => create a new room
  bool create = false;         // true => create; false => join `key`
  // TLS verification.
  std::string ca_file;         // optional CA bundle (dev certs); empty => system roots
  std::string verify_host;     // hostname to verify in the cert; defaults to host
  bool insecure_skip_verify = false;  // DEV ONLY: disable verification
  std::string theme = "dark";  // UI theme name (see client/ui.cpp)
};

// Manages the TLS connection, the create/join handshake, the read loop, and
// automatic reconnection with exponential backoff. All public methods are safe
// to call from the UI thread.
class NetClient {
 public:
  NetClient(ClientConfig cfg, std::function<void(Event)> on_event);
  ~NetClient();

  void start();  // launch the io thread and begin connecting
  void stop();   // graceful shutdown; joins the io thread

  // Send a chat message (encrypted end-to-end). Returns a positive id if handed
  // to the transport (pending an ack), or 0 if not connected (nothing sent, no
  // delivery implied). If `to` is non-empty, it is a direct message.
  long long send_chat(const std::string& text, const std::string& to = "");

  // Request a display-name change. Confirmed via a Nick event when accepted.
  void send_nick(const std::string& new_name);

  // Notify the room that we are typing (server throttles these).
  void send_typing();

  // Ask the server for the current user list.
  void request_users();

  ConnState state() const { return state_.load(); }

 private:
  void emit(Event ev);
  void set_state(ConnState s, const std::string& text);

  void connect();                 // one connection attempt
  void on_resolved(const boost::system::error_code& ec,
                   boost::asio::ip::tcp::resolver::results_type results);
  void on_connected(const boost::system::error_code& ec);
  void on_handshake(const boost::system::error_code& ec);
  void send_hello();              // create or join
  void do_read();
  void on_payload(const std::string& payload);
  void do_write();
  void enqueue(std::string frame);
  void handle_disconnect(const std::string& why);
  void schedule_reconnect();

  ClientConfig cfg_;
  std::function<void(Event)> on_event_;

  boost::asio::io_context io_;
  boost::asio::ssl::context ssl_ctx_;
  boost::asio::ip::tcp::resolver resolver_;
  std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> stream_;
  boost::asio::steady_timer backoff_timer_;

  std::thread io_thread_;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
      work_guard_;

  proto::FrameParser parser_;
  std::string read_buf_;
  std::vector<std::string> pending_frames_;
  std::deque<std::string> write_queue_;
  bool writing_ = false;

  std::atomic<ConnState> state_{ConnState::Connecting};
  std::atomic<bool> stopping_{false};   // suppress reconnection (fatal error or shutdown)
  std::atomic<bool> shutdown_{false};   // stop() teardown has run
  std::atomic<long long> next_id_{1};
  std::chrono::milliseconds backoff_{std::chrono::seconds(1)};
  bool joined_once_ = false;  // affects user-facing wording on reconnect

  // End-to-end encryption. `secret_` is the room key's secret half (never sent
  // to the server); `cipher_` is derived from it.
  std::string secret_;
  std::unique_ptr<crypto::MessageCipher> cipher_;
};

}  // namespace shhterm::client
