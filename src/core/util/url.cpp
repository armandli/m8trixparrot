#include <core/util/url.h>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace util {

namespace {

struct UrlHandleDeleter {
  void operator()(CURLU* handle) const { curl_url_cleanup(handle); }
};
using UrlHandle = std::unique_ptr<CURLU, UrlHandleDeleter>;

// One part of a parsed URL; "" when the URL has none.
std::string part(CURLU* handle, CURLUPart which) {
  char* text = nullptr;
  if (curl_url_get(handle, which, &text, 0) != CURLUE_OK or text == nullptr) {
    return std::string();
  }
  std::string out(text);
  curl_free(text);
  return out;
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

int hex_value(char c) {
  if (c >= '0' and c <= '9') return c - '0';
  if (c >= 'a' and c <= 'f') return c - 'a' + 10;
  if (c >= 'A' and c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::optional<Url> parse_url(std::string_view text, std::string* error) {
  const auto fail = [&](std::string why) -> std::optional<Url> {
    if (error != nullptr) *error = std::move(why);
    return std::nullopt;
  };
  if (text.find('\0') != std::string_view::npos) return fail("the URL contains a NUL byte");

  UrlHandle handle(curl_url());
  if (not handle) return fail("out of memory");
  const std::string copy(text);
  const CURLUcode code = curl_url_set(handle.get(), CURLUPART_URL, copy.c_str(), 0);
  if (code != CURLUE_OK) return fail(curl_url_strerror(code));

  Url url;
  url.scheme = lower(part(handle.get(), CURLUPART_SCHEME));
  url.host = lower(part(handle.get(), CURLUPART_HOST));
  url.port = part(handle.get(), CURLUPART_PORT);
  url.path = part(handle.get(), CURLUPART_PATH);
  url.query = part(handle.get(), CURLUPART_QUERY);
  url.fragment = part(handle.get(), CURLUPART_FRAGMENT);
  url.has_userinfo = not part(handle.get(), CURLUPART_USER).empty() or
                     not part(handle.get(), CURLUPART_PASSWORD).empty();
  if (url.host.empty()) return fail("the URL has no host");
  if (url.path.empty()) url.path = "/";
  return url;
}

std::string url_origin(const Url& url) {
  std::string origin = url.scheme + "://" + url.host;
  const bool default_port = url.port.empty() or
                            (url.scheme == "http" and url.port == "80") or
                            (url.scheme == "https" and url.port == "443");
  if (not default_port) origin += ":" + url.port;
  return origin;
}

std::optional<std::string> resolve_url(std::string_view base, std::string_view reference) {
  UrlHandle handle(curl_url());
  if (not handle) return std::nullopt;
  const std::string base_copy(base);
  const std::string reference_copy(reference);
  if (curl_url_set(handle.get(), CURLUPART_URL, base_copy.c_str(), 0) != CURLUE_OK) {
    return std::nullopt;
  }
  // On a handle that already holds a URL, setting a relative one resolves it.
  if (curl_url_set(handle.get(), CURLUPART_URL, reference_copy.c_str(), 0) != CURLUE_OK) {
    return std::nullopt;
  }
  std::string resolved = part(handle.get(), CURLUPART_URL);
  if (resolved.empty()) return std::nullopt;
  return resolved;
}

bool is_loopback_host(std::string_view host) {
  if (host == "localhost" or host == "[::1]" or host == "::1") return true;
  // 127.0.0.0/8, written as a dotted quad.
  if (host.rfind("127.", 0) != 0) return false;
  int octets = 0;
  size_t at = 0;
  while (at <= host.size()) {
    const size_t dot = std::min(host.find('.', at), host.size());
    const std::string_view octet = host.substr(at, dot - at);
    if (octet.empty() or octet.size() > 3 or
        octet.find_first_not_of("0123456789") != std::string_view::npos or
        std::stoi(std::string(octet)) > 255) {
      return false;
    }
    ++octets;
    at = dot + 1;
  }
  return octets == 4;
}

std::string percent_encode(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte) or c == '-' or c == '.' or c == '_' or c == '~') {
      out += c;
    } else {
      out += '%';
      out += kHex[byte >> 4];
      out += kHex[byte & 0x0F];
    }
  }
  return out;
}

std::string percent_decode(std::string_view text, bool plus_as_space) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '%' and i + 2 < text.size()) {
      const int high = hex_value(text[i + 1]);
      const int low = hex_value(text[i + 2]);
      if (high >= 0 and low >= 0) {
        out += static_cast<char>((high << 4) | low);
        i += 2;
        continue;
      }
    }
    out += (plus_as_space and c == '+') ? ' ' : c;
  }
  return out;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out;
  for (const auto& [key, value] : fields) {
    if (not out.empty()) out += '&';
    out += percent_encode(key);
    out += '=';
    out += percent_encode(value);
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> form_decode(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t at = 0;
  while (at < text.size()) {
    const size_t amp = std::min(text.find('&', at), text.size());
    const std::string_view field = text.substr(at, amp - at);
    if (not field.empty()) {
      const size_t equals = field.find('=');
      const std::string_view key = field.substr(0, equals);
      const std::string_view value =
          equals == std::string_view::npos ? std::string_view() : field.substr(equals + 1);
      out.emplace_back(percent_decode(key, true), percent_decode(value, true));
    }
    at = amp + 1;
  }
  return out;
}

}  // namespace util
