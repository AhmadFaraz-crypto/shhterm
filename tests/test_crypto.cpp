#include <doctest/doctest.h>

#include <string>

#include "crypto.hpp"
#include "protocol.hpp"

using namespace shhterm::client::crypto;

TEST_CASE("base64 round-trips arbitrary bytes") {
  for (std::string s : {std::string(""), std::string("a"), std::string("ab"),
                        std::string("abc"), std::string("hello world"),
                        std::string("\x00\x01\x02\xff\xfe", 5)}) {
    std::string enc =
        base64_encode(reinterpret_cast<const unsigned char*>(s.data()),
                      s.size());
    auto dec = base64_decode(enc);
    REQUIRE(dec.has_value());
    CHECK(*dec == s);
  }
  CHECK_FALSE(base64_decode("@@@not-base64@@@").has_value());
}

TEST_CASE("AES-GCM encrypt/decrypt round-trips with the same secret") {
  std::string secret = shhterm::proto::generate_secret();
  MessageCipher a(secret);
  MessageCipher b(secret);  // peer derives the same key from the shared secret

  std::string msg = "hello, this is a secret 🕵️";
  std::string blob = a.encrypt(msg);

  // Ciphertext is not the plaintext.
  CHECK(blob.find("hello") == std::string::npos);

  auto out = b.decrypt(blob);
  REQUIRE(out.has_value());
  CHECK(*out == msg);
}

TEST_CASE("each encryption uses a fresh nonce (ciphertexts differ)") {
  std::string secret = shhterm::proto::generate_secret();
  MessageCipher c(secret);
  CHECK(c.encrypt("same text") != c.encrypt("same text"));
}

TEST_CASE("a wrong key cannot decrypt (authentication fails)") {
  MessageCipher a(shhterm::proto::generate_secret());
  MessageCipher wrong(shhterm::proto::generate_secret());
  std::string blob = a.encrypt("top secret");
  CHECK_FALSE(wrong.decrypt(blob).has_value());
}

TEST_CASE("tampered ciphertext is rejected") {
  std::string secret = shhterm::proto::generate_secret();
  MessageCipher c(secret);
  std::string blob = c.encrypt("integrity matters");
  // Flip a character in the middle of the base64 blob.
  blob[blob.size() / 2] = (blob[blob.size() / 2] == 'A') ? 'B' : 'A';
  CHECK_FALSE(c.decrypt(blob).has_value());
}

TEST_CASE("end-to-end: compose key -> split -> both sides derive same cipher") {
  // Simulates the real flow: creator makes a secret, server supplies routing,
  // the full key is shared, joiner splits it and derives the same key.
  std::string routing = shhterm::proto::generate_room_key();
  std::string creator_secret = shhterm::proto::generate_secret();
  std::string full = shhterm::proto::compose_room_key(routing, creator_secret);

  auto halves = shhterm::proto::split_room_key(full);
  REQUIRE(halves.has_value());

  MessageCipher creator(creator_secret);
  MessageCipher joiner(halves->second);  // secret extracted from shared key

  std::string blob = creator.encrypt("works across the room");
  auto got = joiner.decrypt(blob);
  REQUIRE(got.has_value());
  CHECK(*got == "works across the room");
}
