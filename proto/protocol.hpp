// shhterm shared wire protocol.
//
// Framing
// -------
// Every message on the wire is a frame:
//
//     [ 4-byte big-endian unsigned length N ][ N bytes of UTF-8 JSON ]
//
// The length prefix counts only the JSON payload, not the prefix itself.
// Frames larger than kMaxFrameSize are rejected and the connection is closed;
// this bounds memory use and provides the "message size limit" guarantee.
//
// Message types (JSON object, field "t")
// --------------------------------------
// Client -> Server:
//   {"t":"create","name":<str>,"ver":<int>}
//   {"t":"join","key":<str>,"name":<str>,"ver":<int>}   // key = ROUTING half only
//   {"t":"msg","id":<int>,"enc":<b64>,"to":<str?>}  // enc = encrypted body; to = optional DM target
//   {"t":"nick","name":<str>}              // request a display-name change
//   {"t":"typing"}                         // user is typing (ephemeral)
//   {"t":"users"}                          // request the current user list
//   {"t":"leave"}                          // graceful departure
//   {"t":"ping"}
//
// Server -> Client:
//   {"t":"welcome","key":<str>,"name":<str>,"users":[<str>...]}  // key = ROUTING half
//   {"t":"msg","from":<str>,"enc":<b64>,"ts":<int>,"dm":<bool?>} // chat broadcast (ciphertext)
//   {"t":"ack","id":<int>}                                       // server accepted & routed msg id
//   {"t":"presence","event":"join"|"leave","name":<str>}        // join/leave notice
//   {"t":"nick","old":<str>,"name":<str>}                       // someone renamed
//   {"t":"typing","from":<str>}                                 // peer is typing
//   {"t":"users","users":[<str>...]}                            // reply to users request
//   {"t":"error","code":<str>,"message":<str>}
//   {"t":"pong"}
//
// error codes: bad_request, bad_version, invalid_key, duplicate_name,
//              not_in_room, rate_limited, too_large, server_full, no_such_user
//
// Security note: TLS protects the transport between each client and the server.
// Message BODIES are additionally end-to-end encrypted by the clients (AES-256-
// GCM; key derived from the room key's secret half, which the server never
// receives) — so the server routes only opaque ciphertext and cannot read
// messages. Metadata the server must act on (usernames, room membership,
// message size and timing) is NOT hidden.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

namespace shhterm::proto {

using json = nlohmann::json;

// Protocol version negotiated at create/join time.
inline constexpr int kProtocolVersion = 1;

// Maximum size of a single framed JSON payload (bytes). Anything larger is a
// protocol violation.
inline constexpr std::uint32_t kMaxFrameSize = 64 * 1024;  // 64 KiB

// Maximum length (in bytes) of a chat message's text field.
inline constexpr std::size_t kMaxTextBytes = 4000;

// Maximum length of a user-chosen display name (bytes).
inline constexpr std::size_t kMaxNameBytes = 32;

// ---- Framing ---------------------------------------------------------------

// Prepend the 4-byte big-endian length prefix to a JSON payload, producing the
// bytes to write to the socket.
std::string encode_frame(const std::string& payload);

// Convenience: serialize a json object and frame it.
std::string encode(const json& message);

// Incremental frame decoder. Feed arbitrary chunks of received bytes; it emits
// complete JSON payloads. Detects oversized frames.
class FrameParser {
 public:
  // Append `n` bytes from `data` and extract any complete frames into `out`.
  // Returns false (and sets error()) if a frame exceeds kMaxFrameSize, after
  // which the parser should not be reused.
  bool feed(const char* data, std::size_t n, std::vector<std::string>& out);

  bool ok() const { return error_.empty(); }
  const std::string& error() const { return error_; }

 private:
  std::string buf_;
  std::string error_;
};

// ---- Room keys (split-key design for end-to-end encryption) ----------------
//
// A shareable room key has two 128-bit halves:
//   - ROUTING half: sent to the server, which uses it to locate the room. The
//     server generates this (generate_room_key) and never learns the secret.
//   - SECRET half: generated and kept by clients only. It is combined into the
//     shared key shown to users, split back out on join, and used to derive the
//     message-encryption key (see client/crypto). The server never receives it.
//
// Each half is 26 Crockford-base32 symbols (128 bits). A full shared key is the
// two halves combined and grouped with dashes for readability, e.g.
// "K4M9-2XQP-7TAE-RJ0F-NB1C-W8G3-...-..".

// Generate the server's 128-bit ROUTING key (grouped, display form).
std::string generate_room_key();

// Generate a client-only 128-bit SECRET half, returned in normalized form
// (26 base32 symbols, no dashes). Throws if the system RNG fails.
std::string generate_secret();

// Combine a routing half and a secret half (either form accepted) into the full
// shared key shown to users, grouped with dashes.
std::string compose_room_key(const std::string& routing,
                             const std::string& secret);

// Split a full shared key (any casing/spacing) into its {routing, secret}
// normalized halves. Returns nullopt if it does not contain exactly two halves.
std::optional<std::pair<std::string, std::string>> split_room_key(
    const std::string& full);

// Canonicalize a key for comparison/lookup: strip non-alphanumerics, uppercase,
// and fold Crockford look-alikes (O->0, I/L->1). Returns "" for input that has
// no usable characters.
std::string normalize_key(const std::string& input);

// ---- Validation helpers ----------------------------------------------------

// True if `name` is non-empty, within length limits, and free of control chars.
bool valid_name(const std::string& name);

// Parse one JSON payload. Returns nullopt if it is not a valid JSON object with
// a string "t" field.
std::optional<json> parse_message(const std::string& payload);

}  // namespace shhterm::proto
