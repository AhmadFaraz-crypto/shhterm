#pragma once

#include <chrono>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include "room.hpp"

namespace shhterm::server {

struct ServerConfig {
  std::string address = "0.0.0.0";
  unsigned short port = 8443;
  std::string cert_file;  // PEM certificate (chain)
  std::string key_file;   // PEM private key
  std::chrono::milliseconds room_ttl = std::chrono::minutes(5);
};

// Accepts TLS connections and hands each to a Session. Runs on the caller's
// io_context (single-threaded by design, so room state needs no locking).
class Server {
 public:
  Server(boost::asio::io_context& io, const ServerConfig& cfg);

  void run();  // start accepting

  // The actual bound port (useful when configured with port 0 to let the OS
  // choose an ephemeral port, e.g. in tests).
  unsigned short port() const { return acceptor_.local_endpoint().port(); }

 private:
  void do_accept();

  boost::asio::io_context& io_;
  boost::asio::ssl::context ssl_ctx_;
  boost::asio::ip::tcp::acceptor acceptor_;
  RoomManager rooms_;
};

}  // namespace shhterm::server
