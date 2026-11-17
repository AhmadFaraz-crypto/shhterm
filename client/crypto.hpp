#pragma once

// Client-side end-to-end encryption for shhterm.
//
// The room key is split into two halves (see proto::split_room_key):
//   - the ROUTING half is sent to the server so it can find the room;
//   - the SECRET half never leaves the client and is used here to derive a
//     symmetric message key.
//
// Because the server only ever receives the routing half, it cannot derive the
// message key and therefore cannot read message contents — it only forwards
// opaque ciphertext. This is genuine end-to-end encryption of message bodies.
// (Metadata the server must act on — usernames, who is in a room, message
// timing/size — is NOT hidden. See the README.)
//
// Scheme: HKDF-SHA256(secret) -> 32-byte key; AES-256-GCM per message with a
// fresh 96-bit random nonce; wire value is base64(nonce || ciphertext || tag).

#include <optional>
#include <string>

namespace shhterm::client::crypto {

// Derives the 32-byte AES key from the room's secret half. The secret is the
// normalized base32 string (already high-entropy); HKDF domain-separates it.
class MessageCipher {
 public:
  explicit MessageCipher(const std::string& room_secret);

  // Encrypt plaintext -> base64(nonce || ciphertext || tag). Throws on failure.
  std::string encrypt(const std::string& plaintext) const;

  // Decrypt a base64 blob produced by encrypt(). Returns nullopt if the input
  // is malformed or authentication fails (wrong key / tampered / truncated).
  std::optional<std::string> decrypt(const std::string& blob) const;

 private:
  unsigned char key_[32];
};

// Base64 helpers (standard alphabet, with padding). Exposed for tests.
std::string base64_encode(const unsigned char* data, std::size_t len);
std::optional<std::string> base64_decode(const std::string& in);

}  // namespace shhterm::client::crypto
