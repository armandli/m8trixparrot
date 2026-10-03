#include <core/util/base64.h>

#include <array>
#include <cstdint>

namespace util {

namespace {

constexpr const char* kStandard =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr const char* kUrlSafe =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// -1 for "not in either alphabet". Both alphabets decode, so a caller never has
// to know which one a server chose.
constexpr std::array<int8_t, 256> make_decode_table() {
  std::array<int8_t, 256> table{};
  for (int8_t& entry : table) entry = -1;
  for (int i = 0; i < 64; ++i) {
    table[static_cast<unsigned char>(kStandard[i])] = static_cast<int8_t>(i);
    table[static_cast<unsigned char>(kUrlSafe[i])] = static_cast<int8_t>(i);
  }
  return table;
}

constexpr std::array<int8_t, 256> kDecode = make_decode_table();

}  // namespace

std::string base64_encode(std::string_view bytes, bool url_safe, bool pad) {
  const char* alphabet = url_safe ? kUrlSafe : kStandard;
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);

  size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const uint32_t chunk =
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
        static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 2]));
    out += alphabet[(chunk >> 18) & 0x3F];
    out += alphabet[(chunk >> 12) & 0x3F];
    out += alphabet[(chunk >> 6) & 0x3F];
    out += alphabet[chunk & 0x3F];
    i += 3;
  }

  const size_t rest = bytes.size() - i;
  if (rest == 1) {
    const uint32_t chunk =
        static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
    out += alphabet[(chunk >> 18) & 0x3F];
    out += alphabet[(chunk >> 12) & 0x3F];
    if (pad) out += "==";
  } else if (rest == 2) {
    const uint32_t chunk =
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
    out += alphabet[(chunk >> 18) & 0x3F];
    out += alphabet[(chunk >> 12) & 0x3F];
    out += alphabet[(chunk >> 6) & 0x3F];
    if (pad) out += '=';
  }
  return out;
}

std::optional<std::string> base64_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size() / 4 * 3);

  uint32_t buffer = 0;
  int bits = 0;
  bool padding = false;
  for (const char c : text) {
    if (c == ' ' or c == '\n' or c == '\r' or c == '\t') continue;
    if (c == '=') {
      padding = true;
      continue;
    }
    // Data after padding is not base64 any more.
    if (padding) return std::nullopt;
    const int8_t value = kDecode[static_cast<unsigned char>(c)];
    if (value < 0) return std::nullopt;
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((buffer >> bits) & 0xFF);
    }
  }
  // A lone trailing sextet carries fewer than 8 bits: not a valid encoding.
  if (bits >= 6) return std::nullopt;
  return out;
}

}  // namespace util
