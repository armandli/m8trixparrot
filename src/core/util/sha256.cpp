#include <core/util/sha256.h>

#include <algorithm>
#include <cstring>

namespace util {

namespace {

constexpr std::array<uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256()
    : mState{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::compress(const uint8_t* block) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 =
        rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 =
        rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = mState[0], b = mState[1], c = mState[2], d = mState[3];
  uint32_t e = mState[4], f = mState[5], g = mState[6], h = mState[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  mState[0] += a;
  mState[1] += b;
  mState[2] += c;
  mState[3] += d;
  mState[4] += e;
  mState[5] += f;
  mState[6] += g;
  mState[7] += h;
}

void Sha256::update(std::string_view bytes) {
  const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
  size_t size = bytes.size();
  mLength += size;

  if (mBuffered > 0) {
    const size_t take = std::min(size, mBuffer.size() - mBuffered);
    std::memcpy(mBuffer.data() + mBuffered, data, take);
    mBuffered += take;
    data += take;
    size -= take;
    if (mBuffered < mBuffer.size()) return;
    compress(mBuffer.data());
    mBuffered = 0;
  }
  while (size >= 64) {
    compress(data);
    data += 64;
    size -= 64;
  }
  if (size > 0) {
    std::memcpy(mBuffer.data(), data, size);
    mBuffered = size;
  }
}

std::array<uint8_t, 32> Sha256::finish() {
  const uint64_t bit_length = mLength * 8;

  // 0x80, zeros up to 56 mod 64, then the big-endian bit length.
  mBuffer[mBuffered++] = 0x80;
  if (mBuffered > 56) {
    while (mBuffered < 64) mBuffer[mBuffered++] = 0;
    compress(mBuffer.data());
    mBuffered = 0;
  }
  while (mBuffered < 56) mBuffer[mBuffered++] = 0;
  for (int i = 7; i >= 0; --i) {
    mBuffer[mBuffered++] = static_cast<uint8_t>(bit_length >> (i * 8));
  }
  compress(mBuffer.data());

  std::array<uint8_t, 32> digest{};
  for (int i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<uint8_t>(mState[i] >> 24);
    digest[i * 4 + 1] = static_cast<uint8_t>(mState[i] >> 16);
    digest[i * 4 + 2] = static_cast<uint8_t>(mState[i] >> 8);
    digest[i * 4 + 3] = static_cast<uint8_t>(mState[i]);
  }
  return digest;
}

std::string sha256_bytes(std::string_view bytes) {
  Sha256 hash;
  hash.update(bytes);
  const std::array<uint8_t, 32> digest = hash.finish();
  return std::string(reinterpret_cast<const char*>(digest.data()),
                     digest.size());
}

std::string sha256_hex(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  const std::string raw = sha256_bytes(bytes);
  std::string out;
  out.reserve(64);
  for (const char c : raw) {
    const auto byte = static_cast<unsigned char>(c);
    out += kHex[byte >> 4];
    out += kHex[byte & 0x0F];
  }
  return out;
}

}  // namespace util
