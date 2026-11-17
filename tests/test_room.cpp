#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>

#include "protocol.hpp"
#include "room.hpp"

using namespace shhterm::server;
namespace proto = shhterm::proto;

namespace {

// Minimal Member that records every frame delivered to it, decoding the JSON
// so tests can assert on routed content.
class FakeMember : public Member {
 public:
  explicit FakeMember(std::string name) : name_(std::move(name)) {}
  const std::string& name() const override { return name_; }
  void deliver(const std::string& frame) override {
    // Each delivered frame is one complete length-prefixed message.
    proto::FrameParser p;
    std::vector<std::string> payloads;
    p.feed(frame.data(), frame.size(), payloads);
    for (auto& payload : payloads) {
      if (auto m = proto::parse_message(payload)) inbox.push_back(*m);
    }
  }
  std::vector<proto::json> inbox;

 private:
  std::string name_;
};

}  // namespace

TEST_CASE("room access: lookup by normalized key, reject unknown keys") {
  boost::asio::io_context io;
  RoomManager mgr(io);

  auto room = mgr.create();
  std::string key = room->display_key();

  // The user may paste the key with different casing / spacing.
  CHECK(mgr.find(proto::normalize_key(key)) == room);
  CHECK(mgr.find(proto::normalize_key("  " + key + "  ")) == room);

  // A wrong key resolves to nothing.
  CHECK(mgr.find(proto::normalize_key("ZZZZ-ZZZZ-ZZZZ")) == nullptr);
}

TEST_CASE("room access: duplicate names are rejected, unique names accepted") {
  boost::asio::io_context io;
  RoomManager mgr(io);
  auto room = mgr.create();

  auto ahmad = std::make_shared<FakeMember>("Ahmad");
  auto ali = std::make_shared<FakeMember>("Ali");
  auto ahmad2 = std::make_shared<FakeMember>("Ahmad");

  CHECK(room->add(ahmad));
  CHECK(room->add(ali));
  CHECK_FALSE(room->add(ahmad2));  // duplicate name
  CHECK(room->size() == 2);
}

TEST_CASE("message routing: broadcast reaches peers but not the sender") {
  boost::asio::io_context io;
  RoomManager mgr(io);
  auto room = mgr.create();

  auto ahmad = std::make_shared<FakeMember>("Ahmad");
  auto ali = std::make_shared<FakeMember>("Ali");
  auto sara = std::make_shared<FakeMember>("Sara");
  room->add(ahmad);
  room->add(ali);
  room->add(sara);

  room->broadcast(proto::encode({{"t", "msg"},
                                 {"from", "Ahmad"},
                                 {"enc", "OPAQUE-CIPHERTEXT"}}),
                  /*except_name=*/"Ahmad");

  CHECK(ahmad->inbox.empty());  // sender excluded
  REQUIRE(ali->inbox.size() == 1);
  REQUIRE(sara->inbox.size() == 1);
  CHECK(ali->inbox[0]["from"] == "Ahmad");
  CHECK(ali->inbox[0]["enc"] == "OPAQUE-CIPHERTEXT");  // server routes blob only
}

TEST_CASE("room rename: unique rename succeeds, collision rejected") {
  boost::asio::io_context io;
  RoomManager mgr(io);
  auto room = mgr.create();
  room->add(std::make_shared<FakeMember>("Ahmad"));
  room->add(std::make_shared<FakeMember>("Ali"));

  CHECK(room->rename("Ahmad", "Ahmed"));   // free name
  CHECK(room->has("Ahmed"));
  CHECK_FALSE(room->has("Ahmad"));
  CHECK_FALSE(room->rename("Ahmed", "Ali"));  // taken
  CHECK_FALSE(room->rename("Ghost", "X"));    // absent
}

TEST_CASE("direct delivery: deliver_to reaches one member only") {
  boost::asio::io_context io;
  RoomManager mgr(io);
  auto room = mgr.create();
  auto a = std::make_shared<FakeMember>("A");
  auto b = std::make_shared<FakeMember>("B");
  room->add(a);
  room->add(b);

  CHECK(room->deliver_to("B", proto::encode({{"t", "msg"}, {"enc", "x"}})));
  CHECK(a->inbox.empty());
  CHECK(b->inbox.size() == 1);
  CHECK_FALSE(room->deliver_to("Nobody", proto::encode({{"t", "msg"}})));
}

TEST_CASE("message routing: two rooms are isolated") {
  boost::asio::io_context io;
  RoomManager mgr(io);
  auto r1 = mgr.create();
  auto r2 = mgr.create();

  auto a = std::make_shared<FakeMember>("A");
  auto b = std::make_shared<FakeMember>("B");
  r1->add(a);
  r2->add(b);

  r1->broadcast(proto::encode({{"t", "msg"}, {"text", "room1 only"}}));
  CHECK(a->inbox.size() == 1);
  CHECK(b->inbox.empty());
}

TEST_CASE("room expiry: deleted after TTL once empty") {
  boost::asio::io_context io;
  RoomManager mgr(io, std::chrono::milliseconds(80));
  auto room = mgr.create();
  auto m = std::make_shared<FakeMember>("solo");
  room->add(m);
  mgr.note_join(room);
  CHECK(mgr.room_count() == 1);

  room->remove("solo");
  mgr.note_leave(room);  // empty -> schedule deletion

  // Before the TTL elapses the room still exists.
  io.run_for(std::chrono::milliseconds(30));
  CHECK(mgr.room_count() == 1);

  // After the TTL it is gone.
  io.run_for(std::chrono::milliseconds(120));
  CHECK(mgr.room_count() == 0);
}

TEST_CASE("room expiry: rejoin within the window cancels deletion") {
  boost::asio::io_context io;
  RoomManager mgr(io, std::chrono::milliseconds(120));
  auto room = mgr.create();
  std::string norm = room->norm_key();

  auto first = std::make_shared<FakeMember>("solo");
  room->add(first);
  mgr.note_join(room);

  // Everyone leaves; deletion is scheduled.
  room->remove("solo");
  mgr.note_leave(room);

  io.run_for(std::chrono::milliseconds(40));  // partway through the window

  // A client reconnects using the same key before expiry.
  auto again = mgr.find(norm);
  REQUIRE(again == room);
  again->add(std::make_shared<FakeMember>("solo"));
  mgr.note_join(again);  // cancels the timer

  io.run_for(std::chrono::milliseconds(150));  // past the original deadline
  CHECK(mgr.room_count() == 1);                // survived
}
