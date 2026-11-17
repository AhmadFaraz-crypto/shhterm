#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>

#include "net_client.hpp"
#include "shhterm_config.hpp"
#include "ui.hpp"

namespace {

using shhterm::client::ClientConfig;

// Load ~/.shhtermrc (simple key=value lines) so users can store their name,
// server, and dev-cert settings once instead of passing flags every time.
// Recognized keys: name, server, ca, verify_host, insecure. CLI flags and the
// SHHTERM_SERVER env var still override these.
std::map<std::string, std::string> load_rc() {
  std::map<std::string, std::string> rc;
  const char* home = std::getenv("HOME");
  if (!home) return rc;
  std::ifstream f(std::string(home) + "/.shhtermrc");
  if (!f) return rc;
  std::string line;
  while (std::getline(f, line)) {
    // Strip comments and surrounding whitespace.
    if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string s) {
      std::size_t a = s.find_first_not_of(" \t\r\n");
      std::size_t b = s.find_last_not_of(" \t\r\n");
      return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    std::string k = trim(line.substr(0, eq));
    std::string v = trim(line.substr(eq + 1));
    if (!k.empty()) rc[k] = v;
  }
  return rc;
}

void print_usage(const char* prog) {
  std::cerr
      << "shhterm — terminal chat over TLS\n\n"
      << "Usage:\n"
      << "  " << prog << " create --name <name> [options]\n"
      << "  " << prog << " join <room-key> --name <name> [options]\n\n"
      << "Options:\n"
      << "  --name <name>        your display name (required)\n"
      << "  --server <host:port> override the server address\n"
      << "                       (env: SHHTERM_SERVER; default: "
      << shhterm::client::kDefaultServer << ")\n"
      << "  --ca <file>          CA certificate to trust (for dev certs)\n"
      << "  --verify-host <name> hostname to verify in the server cert\n"
      << "                       (defaults to the connection host)\n"
      << "  --insecure           DEV ONLY: skip TLS certificate verification\n"
      << "  --theme <name>       color theme: dark, ocean, matrix, sunset, "
         "light, mono\n"
      << "  --help               show this help\n";
}

// Split "host:port" (or "[v6]:port"). Returns false on malformed input.
bool split_hostport(const std::string& in, std::string& host,
                    std::string& port) {
  auto pos = in.rfind(':');
  if (pos == std::string::npos || pos == 0 || pos + 1 == in.size())
    return false;
  host = in.substr(0, pos);
  port = in.substr(pos + 1);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);  // strip IPv6 brackets
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 2;
  }

  ClientConfig cfg;
  std::string mode = argv[1];
  std::string server;  // raw host:port if provided on the CLI

  int i = 2;
  if (mode == "create") {
    cfg.create = true;
  } else if (mode == "join") {
    cfg.create = false;
    if (i < argc && argv[i][0] != '-') {
      cfg.key = argv[i++];
    } else {
      std::cerr << "error: join requires a room key\n";
      print_usage(argv[0]);
      return 2;
    }
  } else if (mode == "--help" || mode == "-h") {
    print_usage(argv[0]);
    return 0;
  } else {
    std::cerr << "error: unknown command '" << mode << "'\n";
    print_usage(argv[0]);
    return 2;
  }

  for (; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "error: " << name << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--name") {
      cfg.name = next("--name");
    } else if (arg == "--server") {
      server = next("--server");
    } else if (arg == "--ca") {
      cfg.ca_file = next("--ca");
    } else if (arg == "--verify-host") {
      cfg.verify_host = next("--verify-host");
    } else if (arg == "--insecure") {
      cfg.insecure_skip_verify = true;
    } else if (arg == "--theme") {
      cfg.theme = next("--theme");
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "error: unknown argument '" << arg << "'\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  // Fill any unset values from ~/.shhtermrc (CLI flags already took priority).
  auto rc = load_rc();
  if (cfg.name.empty() && rc.count("name")) cfg.name = rc["name"];
  if (cfg.theme == "dark" && rc.count("theme")) cfg.theme = rc["theme"];
  if (cfg.ca_file.empty() && rc.count("ca")) cfg.ca_file = rc["ca"];
  if (cfg.verify_host.empty() && rc.count("verify_host"))
    cfg.verify_host = rc["verify_host"];
  if (!cfg.insecure_skip_verify && rc.count("insecure") &&
      (rc["insecure"] == "1" || rc["insecure"] == "true"))
    cfg.insecure_skip_verify = true;

  if (cfg.name.empty()) {
    std::cerr << "error: a name is required (pass --name or set name= in "
                 "~/.shhtermrc)\n";
    print_usage(argv[0]);
    return 2;
  }

  // Resolve server address: --server > $SHHTERM_SERVER > rc file > compiled default.
  if (server.empty()) {
    if (const char* env = std::getenv("SHHTERM_SERVER"); env && *env)
      server = env;
    else if (rc.count("server") && !rc["server"].empty())
      server = rc["server"];
    else
      server = shhterm::client::kDefaultServer;
  }
  if (!split_hostport(server, cfg.host, cfg.port)) {
    std::cerr << "error: invalid server address '" << server
              << "' (expected host:port)\n";
    return 2;
  }
  if (cfg.verify_host.empty()) cfg.verify_host = cfg.host;

  if (cfg.insecure_skip_verify) {
    std::cerr << "WARNING: TLS certificate verification disabled (--insecure). "
                 "Use only for local development.\n";
  }

  // Wire the network layer to the UI. The sink is set before start() so no
  // event can arrive before the UI is ready.
  std::function<void(shhterm::client::Event)> sink;
  shhterm::client::NetClient net(
      cfg, [&sink](shhterm::client::Event e) {
        if (sink) sink(std::move(e));
      });
  shhterm::client::ChatUI ui(net, cfg);
  sink = [&ui](shhterm::client::Event e) { ui.on_net_event(std::move(e)); };

  net.start();
  ui.run();  // blocks until the user quits
  return 0;
}
