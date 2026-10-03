#ifndef M8_UTIL_BASE64_H
#define M8_UTIL_BASE64_H

#include <optional>
#include <string>
#include <string_view>

namespace util {

// RFC 4648 base64. `url_safe` selects the URL alphabet (- and _ for + and /),
// which is what PKCE challenges and JWT segments use; `pad` controls the
// trailing '=' (base64url in OAuth is always unpadded).
std::string base64_encode(std::string_view bytes, bool url_safe = false,
                          bool pad = true);

// Decodes either alphabet, with or without padding. Whitespace is skipped,
// since MCP blob payloads are sometimes line-wrapped. nullopt on anything else
// that is not part of the alphabet.
std::optional<std::string> base64_decode(std::string_view text);

}  // namespace util

#endif  // M8_UTIL_BASE64_H
