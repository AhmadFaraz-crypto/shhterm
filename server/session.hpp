#pragma once

#include <chrono>
#include <deque>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include "member.hpp"
#include "protocol.hpp"
#include "room.hpp"

namespace shhterm::server {

// One connected client. Owns a TLS stream, drives the read/write loops, and
// implements Member so a Room can deliver messages to it.
class Session : public Member,
                public std::enable_shared_from_this<Session> {
 public:
  using ssl_stream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

  Session(ssl_stream stream, RoomManager& rooms);

  // Begin the TLS handshake and, on success, the read loop.
  void start();

  // Member interface.
  const std::string& name() const override { return name_; }
  void deliver(const std::string& frame) override;

 private:
  void do_read();
  void on_frames();
  void handle(const proto::json& msg);

  void handle_create(const proto::json& msg);
  void handle_join(const proto::json& msg);
  void handle_msg(const proto::json& msg);
  void handle_nick(const proto::json& msg);
  void handle_typing();
  void handle_users();
  void leave_room(bool graceful);

  void send(const proto::json& msg);
  void send_error(const std::string& code, const std::string& message);
  void do_write();
  void close();

  // Token-bucket rate limiter for inbound chat messages. Returns true if a
  // message is permitted now and consumes a token.
  bool allow_message();

  ssl_stream stream_;
  RoomManager& rooms_;

  proto::FrameParser parser_;
  std::string read_buf_;
  std::vector<std::string> pending_frames_;

  std::deque<std::string> write_queue_;
  bool writing_ = false;
  bool closed_ = false;

  std::string name_;
  std::shared_ptr<Room> room_;

  // Rate limiter state.
  double tokens_;
  std::chrono::steady_clock::time_point last_refill_;
  // Separate throttle for typing notifications.
  std::chrono::steady_clock::time_point last_typing_{};
};

}  // namespace shhterm::server
