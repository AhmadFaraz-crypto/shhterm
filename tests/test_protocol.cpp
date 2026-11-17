#include <doctest/doctest.h>

#include <cctype>
#include <string>
#include <vector>

#include "protocol.hpp"

using namespace shhterm::proto;

TEST_CASE("frame round-trips through the parser") {
  std::string frame = encode_frame("hello");
  REQUIRE(frame.size() == 4 + 5);
  CHECK(static_cast<unsigned char>(frame[0]) == 0);
  CHECK(static_cast<unsigned char>(frame[3]) == 5);

  FrameParser p;
  std::vector<std::string> out;
  REQUIRE(p.feed(frame.data(), frame.size(), out));
  REQUIRE(out.size() == 1);
  CHECK(out[0] == "hello");
}

TEST_CASE("parser reassembles frames split across reads") {
  std::string a = encode_frame("alpha");
  std::string b = encode_frame("beta");
  std::string stream = a + b;

  FrameParser p;
  std::vector<std::string> out;
  // Feed one byte at a time to stress the incremental path.
  for (char c : stream) {
    REQUIRE(p.feed(&c, 1, out));
  }
  REQUIRE(out.size() == 2);
  CHECK(out[0] == "alpha");
  CHECK(out[1] == "beta");
}

TEST_CASE("parser rejects oversized frames") {
  // Craft a length prefix just over the limit.
  std::uint32_t big = kMaxFrameSize + 1;
  std::string hdr;
  hdr.push_back(static_cast<char>((big >> 24) & 0xFF));
  hdr.push_back(static_cast<char>((big >> 16) & 0xFF));
  hdr.push_back(static_cast<char>((big >> 8) & 0xFF));
  hdr.push_back(static_cast<char>(big & 0xFF));

  FrameParser p;
  std::vector<std::string> out;
  CHECK_FALSE(p.feed(hdr.data(), hdr.size(), out));
  CHECK_FALSE(p.ok());
}

TEST_CASE("parse_message validates structure") {
  CHECK_FALSE(parse_message("not json").has_value());
  CHECK_FALSE(parse_message("[1,2,3]").has_value());       // not an object
  CHECK_FALSE(parse_message("{\"x\":1}").has_value());      // no "t"
  CHECK_FALSE(parse_message("{\"t\":5}").has_value());      // "t" not a string
  auto ok = parse_message("{\"t\":\"ping\"}");
  REQUIRE(ok.has_value());
  CHECK((*ok)["t"] == "ping");
}

TEST_CASE("routing keys carry 128 bits and normalize consistently") {
  std::string key = generate_room_key();
  // 128 bits of Crockford base32 = 26 symbols, plus grouping dashes.
  std::string norm = normalize_key(key);
  CHECK(norm.size() == 26);

  // Two keys differ (astronomically certain).
  CHECK(generate_room_key() != generate_room_key());

  // Normalization folds look-alikes and ignores case/dashes/spacing.
  CHECK(normalize_key("abcd-efgh") == normalize_key("ABCD EFGH"));
  CHECK(normalize_key("O0I1L") == "00111");
  CHECK(normalize_key("!!!") == "");
}

TEST_CASE("split-key: compose and split round-trip, server never sees secret") {
  std::string routing = generate_room_key();   // server's half
  std::string secret = generate_secret();       // client-only half
  CHECK(normalize_key(secret).size() == 26);
  CHECK(secret != generate_secret());           // fresh each time

  std::string full = compose_room_key(routing, secret);
  // The full key the user shares contains BOTH halves (52 symbols normalized).
  CHECK(normalize_key(full).size() == 52);

  auto halves = split_room_key(full);
  REQUIRE(halves.has_value());
  CHECK(halves->first == normalize_key(routing));   // routing recovered
  CHECK(halves->second == normalize_key(secret));   // secret recovered

  // Survives messy user input (lowercase, extra spaces/dashes).
  std::string messy = "  " + full + "  ";
  for (auto& c : messy) c = std::tolower(static_cast<unsigned char>(c));
  auto h2 = split_room_key(messy);
  REQUIRE(h2.has_value());
  CHECK(h2->first == normalize_key(routing));
  CHECK(h2->second == normalize_key(secret));

  // A key of the wrong length is rejected.
  CHECK_FALSE(split_room_key(routing).has_value());  // only 26 symbols
  CHECK_FALSE(split_room_key("too-short").has_value());
}

TEST_CASE("name validation") {
  CHECK(valid_name("Ahmad"));
  CHECK(valid_name("Ali_42"));
  CHECK_FALSE(valid_name(""));
  CHECK_FALSE(valid_name(std::string(100, 'x')));  // too long
  CHECK_FALSE(valid_name("bad\nname"));             // control char
  CHECK_FALSE(valid_name(" leading"));
}
