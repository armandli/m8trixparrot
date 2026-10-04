#include <core/util/text.h>

#include <simdjson.h>

namespace util {

namespace {

constexpr const char* kReplacement = "\xEF\xBF\xBD";  // U+FFFD

// Length of the well-formed sequence starting at text[i], or 0 if the bytes
// there do not begin one. The ranges are RFC 3629's table, which is what rules
// out overlongs (E0 80..9F, F0 80..8F), surrogates (ED A0..BF) and code points
// past U+10FFFF (F4 90.., F5..FF).
size_t sequence_length(std::string_view text, size_t i) {
  const auto at = [&](size_t k) {
    return static_cast<unsigned char>(text[k]);
  };
  const unsigned char lead = at(i);
  const size_t left = text.size() - i;

  if (lead < 0x80) return 1;
  if (lead < 0xC2) return 0;  // stray continuation byte or overlong C0/C1

  const auto continuation = [&](size_t k, unsigned char lo, unsigned char hi) {
    return i + k < text.size() and at(i + k) >= lo and at(i + k) <= hi;
  };

  if (lead < 0xE0) {
    return left >= 2 and continuation(1, 0x80, 0xBF) ? 2 : 0;
  }
  if (lead < 0xF0) {
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    if (lead == 0xE0) lo = 0xA0;
    if (lead == 0xED) hi = 0x9F;
    return left >= 3 and continuation(1, lo, hi) and
                   continuation(2, 0x80, 0xBF)
               ? 3
               : 0;
  }
  if (lead < 0xF5) {
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    if (lead == 0xF0) lo = 0x90;
    if (lead == 0xF4) hi = 0x8F;
    return left >= 4 and continuation(1, lo, hi) and
                   continuation(2, 0x80, 0xBF) and continuation(3, 0x80, 0xBF)
               ? 4
               : 0;
  }
  return 0;
}

}  // namespace

bool is_valid_utf8(std::string_view text) {
  return simdjson::validate_utf8(text.data(), text.size());
}

std::string sanitize_utf8(std::string_view text) {
  // The common case is clean text; simdjson answers that at memory speed.
  if (is_valid_utf8(text)) return std::string(text);

  std::string out;
  out.reserve(text.size() + 8);
  size_t i = 0;
  while (i < text.size()) {
    const size_t n = sequence_length(text, i);
    if (n == 0) {
      // One replacement per offending byte, so a run of garbage stays visibly a
      // run rather than collapsing into a single character.
      out += kReplacement;
      ++i;
      continue;
    }
    out.append(text.data() + i, n);
    i += n;
  }
  return out;
}

int64_t estimate_tokens(std::string_view text) {
  return static_cast<int64_t>((text.size() + 3) / 4);
}

}  // namespace util
