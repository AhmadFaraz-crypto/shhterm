#include "protocol.hpp"

#include <openssl/rand.h>

#include <array>
#include <cstring>
#include <stdexcept>

namespace shhterm::proto {

namespace {

// Crockford base32 alphabet (excludes I, L, O, U to reduce ambiguity).
constexpr char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::string crockford_base32(const std::uint8_t* data, std::size_t len) {
  std::string out;
  int buffer = 0;
  int bits = 0;
  for (std::size_t i = 0; i < len; ++i) {
    buffer = (buffer << 8) | data[i];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out.push_back(kCrockford[(buffer >> bits) & 0x1F]);
    }
  }
  if (bits > 0) {
    out.push_back(kCrockford[(buffer << (5 - bits)) & 0x1F]);
  }
  return out;
}

}  // namespace

// ---- Framing ---------------------------------------------------------------

std::string encode_frame(const std::string& payload) {
  const std::uint32_t n = static_cast<std::uint32_t>(payload.size());
  std::string out;
  out.reserve(4 + payload.size());
  out.push_back(static_cast<char>((n >> 24) & 0xFF));
  out.push_back(static_cast<char>((n >> 16) & 0xFF));
  out.push_back(static_cast<char>((n >> 8) & 0xFF));
  out.push_back(static_cast<char>(n & 0xFF));
  out.append(payload);
  return out;
}

std::string encode(const json& message) {
  return encode_frame(message.dump());
}

bool FrameParser::feed(const char* data, std::size_t n,
                       std::vector<std::string>& out) {
  if (!error_.empty()) return false;
  buf_.append(data, n);

  for (;;) {
    if (buf_.size() < 4) break;  // need the length prefix
    const std::uint32_t len =
        (static_cast<std::uint8_t>(buf_[0]) << 24) |
        (static_cast<std::uint8_t>(buf_[1]) << 16) |
        (static_cast<std::uint8_t>(buf_[2]) << 8) |
        (static_cast<std::uint8_t>(buf_[3]));
    if (len > kMaxFrameSize) {
      error_ = "frame exceeds maximum size";
      return false;
    }
    if (buf_.size() < 4 + static_cast<std::size_t>(len)) break;  // incomplete
    out.emplace_back(buf_.substr(4, len));
    buf_.erase(0, 4 + len);
  }
  return true;
}

// ---- Room keys -------------------------------------------------------------

namespace {

// Group a normalized key string into dash-separated blocks of 4 for display.
std::string group(const std::string& raw) {
  std::string out;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (i > 0 && i % 4 == 0) out.push_back('-');
    out.push_back(raw[i]);
  }
  return out;
}

// Generate a normalized 128-bit half: 26 Crockford base32 symbols.
std::string random_half() {
  std::array<std::uint8_t, 16> bytes{};  // 128 bits
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    throw std::runtime_error("secure RNG (RAND_bytes) failed");
  }
  return crockford_base32(bytes.data(), bytes.size());  // 26 chars
}

}  // namespace

std::string generate_room_key() { return group(random_half()); }

std::string generate_secret() { return random_half(); }

std::string compose_room_key(const std::string& routing,
                             const std::string& secret) {
  return group(normalize_key(routing) + normalize_key(secret));
}

std::optional<std::pair<std::string, std::string>> split_room_key(
    const std::string& full) {
  std::string norm = normalize_key(full);
  if (norm.size() != 52) return std::nullopt;  // two 26-symbol halves
  return std::make_pair(norm.substr(0, 26), norm.substr(26));
}

std::string normalize_key(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc >= 'a' && uc <= 'z') uc = static_cast<unsigned char>(uc - 'a' + 'A');
    if (!((uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9'))) continue;
    switch (uc) {
      case 'O': uc = '0'; break;
      case 'I': uc = '1'; break;
      case 'L': uc = '1'; break;
      default: break;
    }
    out.push_back(static_cast<char>(uc));
  }
  return out;
}

// ---- Validation ------------------------------------------------------------

bool valid_name(const std::string& name) {
  if (name.empty() || name.size() > kMaxNameBytes) return false;
  for (char c : name) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc == 0x7F) return false;  // reject control characters
  }
  // Disallow leading/trailing whitespace to keep names tidy.
  if (name.front() == ' ' || name.back() == ' ') return false;
  return true;
}

std::optional<json> parse_message(const std::string& payload) {
  json j = json::parse(payload, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return std::nullopt;
  auto it = j.find("t");
  if (it == j.end() || !it->is_string()) return std::nullopt;
  return j;
}

}  // namespace shhterm::proto
