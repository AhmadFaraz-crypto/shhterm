// Tiny CLI around the C++ MessageCipher, used to verify that the browser
// (Web Crypto) end-to-end scheme is byte-compatible with the terminal client.
//
//   crypto_tool encrypt <secret> <plaintext>   -> prints base64 blob
//   crypto_tool decrypt <secret> <blob>        -> prints plaintext, or "FAIL"
//
// Not shipped to users; a verification aid only.

#include <iostream>
#include <string>

#include "crypto.hpp"

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: crypto_tool encrypt|decrypt <secret> <data>\n";
    return 2;
  }
  std::string mode = argv[1], secret = argv[2], data = argv[3];
  shhterm::client::crypto::MessageCipher cipher(secret);
  if (mode == "encrypt") {
    std::cout << cipher.encrypt(data) << "\n";
  } else if (mode == "decrypt") {
    auto pt = cipher.decrypt(data);
    if (!pt) {
      std::cout << "FAIL\n";
      return 1;
    }
    std::cout << *pt << "\n";
  } else {
    std::cerr << "unknown mode\n";
    return 2;
  }
  return 0;
}
