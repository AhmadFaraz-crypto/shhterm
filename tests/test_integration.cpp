// End-to-end tests: a real TLS server plus real NetClients, exercising the
// full stack (framing, TLS, create/join, routing, acks, presence) over
// loopback. A throwaway self-signed certificate is generated with the system
// `openssl` CLI; tests are skipped if it is unavailable.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "net_client.hpp"
#include "server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace tc = shhterm::client;

namespace {

struct DevCert {
  fs::path cert, key;
  bool ok = false;
};

DevCert make_dev_cert() {
  DevCert c;
  fs::path dir = fs::temp_directory_path() / "shhterm_it";
  std::error_code ec;
  fs::create_directories(dir, ec);
  c.cert = dir / "cert.pem";
  c.key = dir / "key.pem";
  std::string cmd =
      "openssl req -x509 -newkey rsa:2048 -nodes -days 1 "
      "-keyout '" + c.key.string() + "' -out '" + c.cert.string() + "' "
      "-subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost' "
      "> /dev/null 2>&1";
  c.ok = (std::system(cmd.c_str()) == 0) && fs::exists(c.cert) &&
         fs::exists(c.key);
  return c;
}

// Thread-safe sink that records NetClient events and lets tests wait for them.
class Recorder {
 public:
  std::function<void(tc::Event)> sink() {
    return [this](tc::Event e) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        events_.push_back(std::move(e));
      }
      cv_.notify_all();
    };
  }

  // Wait until `pred` is satisfied over the collected events, or timeout.
  template <class Pred>
  bool wait_for(Pred pred, std::chrono::milliseconds to = 5000ms) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, to, [&] { return pred(events_); });
  }

  std::vector<tc::Event> snapshot() {
    std::lock_guard<std::mutex> lk(mu_);
    return events_;
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<tc::Event> events_;
};

std::string find_key(const std::vector<tc::Event>& evs) {
  for (const auto& e : evs)
    if (e.type == tc::Event::Type::Welcome) return e.key;
  return {};
}

}  // namespace

TEST_CASE("end-to-end over TLS: create, join, route, ack, presence") {
  DevCert cert = make_dev_cert();
  if (!cert.ok) {
    MESSAGE("openssl CLI unavailable; skipping TLS integration test");
    return;
  }

  // ---- Start the server on an OS-chosen port in its own io thread. ----
  boost::asio::io_context server_io;
  shhterm::server::ServerConfig scfg;
  scfg.address = "127.0.0.1";
  scfg.port = 0;  // ephemeral
  scfg.cert_file = cert.cert.string();
  scfg.key_file = cert.key.string();
  scfg.room_ttl = 500ms;

  shhterm::server::Server server(server_io, scfg);
  server.run();
  unsigned short port = server.port();
  std::thread server_thread([&] { server_io.run(); });

  auto base_cfg = [&](const std::string& name, const std::string& key,
                      bool create) {
    tc::ClientConfig cc;
    cc.host = "127.0.0.1";
    cc.port = std::to_string(port);
    cc.name = name;
    cc.key = key;
    cc.create = create;
    cc.ca_file = cert.cert.string();  // trust our dev CA
    cc.verify_host = "localhost";     // cert SAN
    return cc;
  };

  // ---- Client A creates a room. ----
  Recorder recA;
  tc::NetClient a(base_cfg("Ahmad", "", /*create=*/true), recA.sink());
  a.start();
  REQUIRE(recA.wait_for([](auto& evs) { return !find_key(evs).empty(); }));
  std::string key = find_key(recA.snapshot());
  CHECK_FALSE(key.empty());

  // ---- Client B joins with the shared key. ----
  Recorder recB;
  tc::NetClient b(base_cfg("Ali", key, /*create=*/false), recB.sink());
  b.start();
  REQUIRE(recB.wait_for([](auto& evs) {
    for (auto& e : evs)
      if (e.type == tc::Event::Type::Welcome) return true;
    return false;
  }));

  // A should observe B's join via presence.
  CHECK(recA.wait_for([](auto& evs) {
    for (auto& e : evs)
      if (e.type == tc::Event::Type::Presence && e.from == "Ali") return true;
    return false;
  }));

  // ---- A sends a message; B receives it; A gets an ack. ----
  long long id = a.send_chat("hello over TLS");
  CHECK(id > 0);
  CHECK(recB.wait_for([](auto& evs) {
    for (auto& e : evs)
      if (e.type == tc::Event::Type::Chat && e.from == "Ahmad" &&
          e.text == "hello over TLS")
        return true;
    return false;
  }));
  CHECK(recA.wait_for([id](auto& evs) {
    for (auto& e : evs)
      if (e.type == tc::Event::Type::Ack && e.id == id) return true;
    return false;
  }));

  a.stop();
  b.stop();
  server_io.stop();
  server_thread.join();
}

TEST_CASE("end-to-end over TLS: joining with a bad key fails cleanly") {
  DevCert cert = make_dev_cert();
  if (!cert.ok) return;

  boost::asio::io_context server_io;
  shhterm::server::ServerConfig scfg;
  scfg.address = "127.0.0.1";
  scfg.port = 0;
  scfg.cert_file = cert.cert.string();
  scfg.key_file = cert.key.string();
  shhterm::server::Server server(server_io, scfg);
  server.run();
  unsigned short port = server.port();
  std::thread server_thread([&] { server_io.run(); });

  tc::ClientConfig cc;
  cc.host = "127.0.0.1";
  cc.port = std::to_string(port);
  cc.name = "Nobody";
  cc.key = "ZZZZ-ZZZZ-ZZZZ-ZZZZ-ZZZZ-ZZ";
  cc.create = false;
  cc.ca_file = cert.cert.string();
  cc.verify_host = "localhost";

  Recorder rec;
  tc::NetClient c(cc, rec.sink());
  c.start();

  // Expect a Notice carrying the invalid_key error and a Failed status.
  CHECK(rec.wait_for([](auto& evs) {
    for (auto& e : evs)
      if (e.type == tc::Event::Type::Notice &&
          e.text.find("invalid_key") != std::string::npos)
        return true;
    return false;
  }));

  c.stop();
  server_io.stop();
  server_thread.join();
}
