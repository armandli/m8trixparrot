#include <core/vdb/byte_io.h>

#include <array>

namespace vdb {

namespace {

// The Castagnoli polynomial in reversed bit order (0x1EDC6F41 reflected).
inline constexpr uint32_t kCrc32cPoly = 0x82F63B78u;

std::array<uint32_t, 256> build_crc_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ kCrc32cPoly : crc >> 1;
    }
    table[i] = crc;
  }
  return table;
}

const std::array<uint32_t, 256>& crc_table() {
  static const std::array<uint32_t, 256> table = build_crc_table();
  return table;
}

}  // namespace

uint32_t crc32c(std::string_view data, uint32_t seed) {
  const std::array<uint32_t, 256>& table = crc_table();
  uint32_t crc = ~seed;
  for (const char c : data) {
    crc = table[(crc ^ static_cast<uint8_t>(c)) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

}  // namespace vdb
