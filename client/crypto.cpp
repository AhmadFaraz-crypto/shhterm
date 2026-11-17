#include "crypto.hpp"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <array>
#include <cstring>
#include <stdexcept>

namespace shhterm::client::crypto {

namespace {

constexpr int kNonceLen = 12;  // 96-bit GCM nonce
constexpr int kTagLen = 16;    // 128-bit GCM tag
constexpr char kHkdfInfo[] = "shhterm-e2e-v1";

// HKDF-SHA256 expand of `secret` into a 32-byte key.
void derive_key(const std::string& secret, unsigned char out[32]) {
  EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (!pctx) throw std::runtime_error("HKDF ctx alloc failed");
  std::size_t outlen = 32;
  bool ok =
      EVP_PKEY_derive_init(pctx) > 0 &&
      EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) > 0 &&
      EVP_PKEY_CTX_set1_hkdf_salt(pctx,
                                  reinterpret_cast<const unsigned char*>(""),
                                  0) > 0 &&
      EVP_PKEY_CTX_set1_hkdf_key(
          pctx, reinterpret_cast<const unsigned char*>(secret.data()),
          static_cast<int>(secret.size())) > 0 &&
      EVP_PKEY_CTX_add1_hkdf_info(
          pctx, reinterpret_cast<const unsigned char*>(kHkdfInfo),
          static_cast<int>(sizeof(kHkdfInfo) - 1)) > 0 &&
      EVP_PKEY_derive(pctx, out, &outlen) > 0 && outlen == 32;
  EVP_PKEY_CTX_free(pctx);
  if (!ok) throw std::runtime_error("HKDF derive failed");
}

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string base64_encode(const unsigned char* data, std::size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  std::size_t i = 0;
  for (; i + 2 < len; i += 3) {
    unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out.push_back(kB64[(v >> 18) & 0x3F]);
    out.push_back(kB64[(v >> 12) & 0x3F]);
    out.push_back(kB64[(v >> 6) & 0x3F]);
    out.push_back(kB64[v & 0x3F]);
  }
  if (i < len) {
    unsigned v = data[i] << 16;
    bool two = (i + 1 < len);
    if (two) v |= data[i + 1] << 8;
    out.push_back(kB64[(v >> 18) & 0x3F]);
    out.push_back(kB64[(v >> 12) & 0x3F]);
    out.push_back(two ? kB64[(v >> 6) & 0x3F] : '=');
    out.push_back('=');
  }
  return out;
}

std::optional<std::string> base64_decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string stripped;
  stripped.reserve(in.size());
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    stripped.push_back(c);
  }
  std::string out;
  out.reserve(stripped.size() * 3 / 4);
  int buffer = 0, bits = 0;
  for (char c : stripped) {
    int v = val(c);
    if (v < 0) return std::nullopt;  // invalid character
    buffer = (buffer << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

MessageCipher::MessageCipher(const std::string& room_secret) {
  derive_key(room_secret, key_);
}

std::string MessageCipher::encrypt(const std::string& plaintext) const {
  unsigned char nonce[kNonceLen];
  if (RAND_bytes(nonce, kNonceLen) != 1)
    throw std::runtime_error("RAND_bytes failed");

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) throw std::runtime_error("cipher ctx alloc failed");

  std::string ct(plaintext.size(), '\0');
  unsigned char tag[kTagLen];
  int len = 0, total = 0;
  bool ok =
      EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) ==
          1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr) ==
          1 &&
      EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_, nonce) == 1 &&
      EVP_EncryptUpdate(
          ctx, reinterpret_cast<unsigned char*>(&ct[0]), &len,
          reinterpret_cast<const unsigned char*>(plaintext.data()),
          static_cast<int>(plaintext.size())) == 1;
  total = len;
  ok = ok &&
       EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&ct[0]) + total,
                           &len) == 1;
  total += len;
  ok = ok &&
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) throw std::runtime_error("AES-GCM encryption failed");

  // Assemble nonce || ciphertext || tag and base64 it.
  std::string packed;
  packed.reserve(kNonceLen + total + kTagLen);
  packed.append(reinterpret_cast<char*>(nonce), kNonceLen);
  packed.append(ct.data(), total);
  packed.append(reinterpret_cast<char*>(tag), kTagLen);
  return base64_encode(reinterpret_cast<const unsigned char*>(packed.data()),
                       packed.size());
}

std::optional<std::string> MessageCipher::decrypt(
    const std::string& blob) const {
  auto packed = base64_decode(blob);
  if (!packed) return std::nullopt;
  if (packed->size() < static_cast<std::size_t>(kNonceLen + kTagLen))
    return std::nullopt;

  const unsigned char* data =
      reinterpret_cast<const unsigned char*>(packed->data());
  const unsigned char* nonce = data;
  const unsigned char* ct = data + kNonceLen;
  std::size_t ct_len = packed->size() - kNonceLen - kTagLen;
  const unsigned char* tag = data + kNonceLen + ct_len;

  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return std::nullopt;

  std::string pt(ct_len, '\0');
  int len = 0, total = 0, rc = 0;
  bool ok =
      EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) ==
          1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr) ==
          1 &&
      EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_, nonce) == 1 &&
      EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&pt[0]), &len, ct,
                        static_cast<int>(ct_len)) == 1;
  total = len;
  if (ok) {
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen,
                        const_cast<unsigned char*>(tag));
    rc = EVP_DecryptFinal_ex(
        ctx, reinterpret_cast<unsigned char*>(&pt[0]) + total, &len);
    total += len;
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok || rc != 1) return std::nullopt;  // authentication failure
  pt.resize(total);
  return pt;
}

}  // namespace shhterm::client::crypto
