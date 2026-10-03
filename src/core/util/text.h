#ifndef M8_UTIL_TEXT_H
#define M8_UTIL_TEXT_H

#include <cstdint>
#include <string>
#include <string_view>

namespace util {

// True when `text` is well-formed UTF-8 (RFC 3629: no overlong forms, no
// surrogates, nothing past U+10FFFF).
bool is_valid_utf8(std::string_view text);

// `text` with every ill-formed byte sequence replaced by U+FFFD. Valid input is
// returned unchanged.
//
// This matters more than it looks: util::JsonWriter refuses invalid UTF-8 by
// producing an EMPTY document, so one stray byte from a subprocess or an MCP
// server, carried into the transcript, would empty every later /api/chat body
// rather than corrupt one character. Anything that did not come from a JSON
// parser goes through here before it can reach a request.
std::string sanitize_utf8(std::string_view text);

// The cheap `chars / 4` token estimate the rest of the code base uses, for text
// that is never sent anywhere to be counted properly (tool schemas, catalog
// listings).
int64_t estimate_tokens(std::string_view text);

}  // namespace util

#endif  // M8_UTIL_TEXT_H
