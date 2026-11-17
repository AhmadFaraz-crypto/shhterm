#include <cstdlib>
#include <iostream>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include "server.hpp"

namespace {

using shhterm::server::ServerConfig;

void print_usage(const char* prog) {
  std::cerr
      << "Usage: " << prog << " [options]\n"
      << "  --address <ip>       bind address (default 0.0.0.0)\n"
      << "  --port <n>           listen port (default 8443)\n"
      << "  --cert <file>        PEM certificate chain (required)\n"
      << "  --key <file>         PEM private key (required)\n"
      << "  --ttl-seconds <n>    empty-room lifetime before deletion "
         "(default 300)\n"
      << "  --help               show this help\n";
}

// Returns false if parsing failed (message already printed).
bool parse_args(int argc, char** argv, ServerConfig& cfg) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "error: " << name << " requires a value\n";
        return {};
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (arg == "--address") {
      cfg.address = next("--address");
    } else if (arg == "--port") {
      cfg.port = static_cast<unsigned short>(std::stoi(next("--port")));
    } else if (arg == "--cert") {
      cfg.cert_file = next("--cert");
    } else if (arg == "--key") {
      cfg.key_file = next("--key");
    } else if (arg == "--ttl-seconds") {
      cfg.room_ttl = std::chrono::seconds(std::stoi(next("--ttl-seconds")));
    } else {
      std::cerr << "error: unknown argument '" << arg << "'\n";
      print_usage(argv[0]);
      return false;
    }
  }
  if (cfg.cert_file.empty() || cfg.key_file.empty()) {
    std::cerr << "error: --cert and --key are required\n";
    print_usage(argv[0]);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  ServerConfig cfg;
  if (!parse_args(argc, argv, cfg)) return 2;

  try {
    boost::asio::io_context io;

    // Graceful shutdown on SIGINT/SIGTERM.
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
      std::cout << "\nshutting down\n";
      io.stop();
    });

    shhterm::server::Server server(io, cfg);
    server.run();
    io.run();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
