#include "server.hpp"

#include <iostream>
#include <utility>

#include <boost/asio/ip/address.hpp>

#include "session.hpp"

namespace shhterm::server {

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using asio::ip::tcp;

Server::Server(asio::io_context& io, const ServerConfig& cfg)
    : io_(io),
      ssl_ctx_(ssl::context::tls_server),
      acceptor_(io),
      rooms_(io, cfg.room_ttl) {
  // Require modern TLS only.
  ssl_ctx_.set_options(ssl::context::default_workarounds |
                       ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                       ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1 |
                       ssl::context::single_dh_use);
  ssl_ctx_.use_certificate_chain_file(cfg.cert_file);
  ssl_ctx_.use_private_key_file(cfg.key_file, ssl::context::pem);

  tcp::endpoint endpoint(asio::ip::make_address(cfg.address), cfg.port);
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(asio::socket_base::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();
  std::cout << "shhterm server listening on " << cfg.address << ":"
            << cfg.port << " (room TTL "
            << std::chrono::duration_cast<std::chrono::seconds>(cfg.room_ttl)
                   .count()
            << "s)\n";
}

void Server::run() { do_accept(); }

void Server::do_accept() {
  acceptor_.async_accept([this](const boost::system::error_code& ec,
                                tcp::socket socket) {
    if (!ec) {
      auto session = std::make_shared<Session>(
          Session::ssl_stream(std::move(socket), ssl_ctx_), rooms_);
      session->start();
    }
    // Keep accepting regardless of a single failed accept.
    do_accept();
  });
}

}  // namespace shhterm::server
