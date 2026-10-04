#ifndef M8_UTIL_SHA256_H
#define M8_UTIL_SHA256_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace util {

// FIPS 180-4 SHA-256, written out here because the project links no crypto
// library of its own (libcurl's TLS backend is not an API). Used for PKCE
// challenges, MCP server approval fingerprints and collision suffixes — none of
// which are performance sensitive.
struct Sha256 {
  Sha256();
  void update(std::string_view bytes);
  // The 32-byte digest. The object should not be updated afterwards.
  std::array<uint8_t, 32> finish();

private:
  void compress(const uint8_t* block);

  std::array<uint32_t, 8> mState;
  std::array<uint8_t, 64> mBuffer{};
  size_t mBuffered = 0;
  uint64_t mLength = 0;  // bytes hashed so far
};

// The raw 32 bytes, for base64url encoding (PKCE S256).
std::string sha256_bytes(std::string_view bytes);
// Lowercase hex, for fingerprints people may read.
std::string sha256_hex(std::string_view bytes);

}  // namespace util

#endif  // M8_UTIL_SHA256_H
