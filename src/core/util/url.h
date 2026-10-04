#ifndef M8_UTIL_URL_H
#define M8_UTIL_URL_H

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace util {

// A URL taken apart by libcurl's own parser (curl_url), so m8 and the library
// that fetches the URL agree about where it points — two parsers disagreeing
// over a crafted URL is how a check gets passed and then bypassed.
struct Url {
  std::string scheme;    // lowercase: "https"
  std::string host;      // lowercase; an IPv6 literal keeps its brackets
  std::string port;      // "" when the URL names none
  std::string path;      // at least "/"
  std::string query;     // without the '?'; "" when there is none
  std::string fragment;  // without the '#'
  bool has_userinfo = false;
};

// nullopt, with why in `error`, unless `text` is an absolute URL with a host
// that libcurl accepts.
std::optional<Url> parse_url(std::string_view text, std::string* error = nullptr);

// scheme://host[:port], as RFC 6454 defines an origin; a port that is the
// scheme's default (80, 443) is left out, so equal origins compare equal.
std::string url_origin(const Url& url);

// `reference` resolved against `base` (RFC 3986 section 5), via libcurl;
// nullopt when either cannot be parsed.
std::optional<std::string> resolve_url(std::string_view base, std::string_view reference);

// localhost, 127.0.0.0/8 and ::1: where plain http never leaves the machine.
bool is_loopback_host(std::string_view host);

// RFC 3986 percent-encoding of everything but the unreserved characters
// (ALPHA, DIGIT, "-", ".", "_", "~").
std::string percent_encode(std::string_view text);
// The reverse. A '%' not followed by two hex digits is kept as written;
// `plus_as_space` reads '+' as a space, as form encoding does.
std::string percent_decode(std::string_view text, bool plus_as_space = false);

// application/x-www-form-urlencoded: a=b&c=d, both sides percent-encoded.
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields);
// A query string or form body back into pairs, in order.
std::vector<std::pair<std::string, std::string>> form_decode(std::string_view text);

}  // namespace util

#endif  // M8_UTIL_URL_H
