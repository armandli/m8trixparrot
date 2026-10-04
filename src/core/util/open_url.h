#ifndef M8_UTIL_OPEN_URL_H
#define M8_UTIL_OPEN_URL_H

#include <string>

namespace util {

// Opens an http(s) URL in the user's browser: `open` on macOS, `xdg-open`
// elsewhere, run directly with the URL as one argument — never through a
// shell, so nothing in a URL a server chose can become a command. False, with
// why, when the URL is not plain http(s) or the opener failed.
bool open_url(const std::string& url, std::string& error);

// The checks open_url makes before it runs anything; empty when `url` may be
// opened.
std::string open_url_problem(const std::string& url);

}  // namespace util

#endif  // M8_UTIL_OPEN_URL_H
