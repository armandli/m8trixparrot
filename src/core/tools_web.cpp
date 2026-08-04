#include <core/tools.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <curl/curl.h>

#include <core/tools_util.hpp>

namespace agent {

namespace {

// A page bigger than this is almost certainly not something a model wants to
// read; the transfer is cut off rather than buffered to exhaustion.
constexpr size_t kMaxDownloadBytes = 10u * 1024u * 1024u;

// Plenty of sites reject libcurl's default user agent outright.
constexpr const char* kUserAgent =
    "Mozilla/5.0 (compatible; m8trixparrot/1.0; +https://github.com/armandli/m8trixparrot)";

// curl_global_init() must run once before any handle is used and is not itself
// thread-safe, so it happens through a function-local static — the same idiom
// as ollama_client.cpp.
void ensure_curl_initialized() {
  static const CURLcode init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  (void)init_result;
}

// ---------------------------------------------------------------------------
// Download sink.
// ---------------------------------------------------------------------------

struct DownloadSink {
  std::string data;
  bool capped = false;
};

// Returning less than the offered byte count makes curl abort the transfer with
// CURLE_WRITE_ERROR, which the caller reads as "hit the cap" rather than as a
// failure.
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* sink = static_cast<DownloadSink*>(userdata);
  const size_t bytes = size * nmemb;

  if (sink->data.size() + bytes > kMaxDownloadBytes) {
    const size_t room = kMaxDownloadBytes - sink->data.size();
    sink->data.append(ptr, room);
    sink->capped = true;
    return 0;
  }

  sink->data.append(ptr, bytes);
  return bytes;
}

// ---------------------------------------------------------------------------
// Small string helpers.
// ---------------------------------------------------------------------------

std::string to_lower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lowered;
}

bool is_space(char c) {
  return c == ' ' or c == '\t' or c == '\r' or c == '\n' or c == '\f' or
         c == '\v';
}

std::string trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end and is_space(text[begin])) ++begin;
  while (end > begin and is_space(text[end - 1])) --end;
  return std::string(text.substr(begin, end - begin));
}

// ---------------------------------------------------------------------------
// URL resolution.
//
// Relative hrefs are worth resolving: a model that reads "see [docs](/guide)"
// can act on it, while a bare "/guide" is a dead end.
// ---------------------------------------------------------------------------

bool has_scheme(std::string_view url) {
  size_t i = 0;
  while (i < url.size() and (std::isalnum(static_cast<unsigned char>(url[i])) or
                             url[i] == '+' or url[i] == '-' or url[i] == '.')) {
    ++i;
  }
  return i > 0 and i < url.size() and url[i] == ':';
}

// "https://host:port" of `base`, or empty if base has no scheme+authority.
std::string origin_of(const std::string& base) {
  const size_t scheme_end = base.find("://");
  if (scheme_end == std::string::npos) return std::string();

  const size_t authority_end = base.find('/', scheme_end + 3);
  if (authority_end == std::string::npos) return base;
  return base.substr(0, authority_end);
}

// `base` with everything after its last '/' removed, i.e. the directory a
// plain relative path hangs off.
std::string directory_of(const std::string& base) {
  const size_t scheme_end = base.find("://");
  const size_t search_from = scheme_end == std::string::npos ? 0 : scheme_end + 3;

  const size_t last_slash = base.find_last_of('/');
  if (last_slash == std::string::npos or last_slash < search_from) {
    return base + "/";
  }
  return base.substr(0, last_slash + 1);
}

std::string resolve_url(const std::string& base, std::string_view href) {
  const std::string target = trim(href);
  if (target.empty()) return std::string();

  // Fragments and javascript: hrefs lead nowhere useful in a text rendering.
  if (target[0] == '#') return std::string();
  if (to_lower(target).rfind("javascript:", 0) == 0) return std::string();

  if (target.rfind("//", 0) == 0) {
    const size_t scheme_end = base.find("://");
    const std::string scheme =
        scheme_end == std::string::npos ? "https" : base.substr(0, scheme_end);
    return scheme + ":" + target;
  }

  if (has_scheme(target)) return target;
  if (base.empty()) return target;

  if (target[0] == '/') {
    const std::string origin = origin_of(base);
    return origin.empty() ? target : origin + target;
  }

  return directory_of(base) + target;
}

// ---------------------------------------------------------------------------
// HTML entities.
// ---------------------------------------------------------------------------

void append_utf8(std::string& out, uint32_t code_point) {
  if (code_point < 0x80) {
    out += static_cast<char>(code_point);
  } else if (code_point < 0x800) {
    out += static_cast<char>(0xC0 | (code_point >> 6));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  } else if (code_point < 0x10000) {
    out += static_cast<char>(0xE0 | (code_point >> 12));
    out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code_point >> 18));
    out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  }
}

// The handful of named entities that actually show up in prose. Anything else
// is left as written rather than guessed at.
std::optional<std::string> named_entity(std::string_view name) {
  if (name == "amp") return std::string("&");
  if (name == "lt") return std::string("<");
  if (name == "gt") return std::string(">");
  if (name == "quot") return std::string("\"");
  if (name == "apos") return std::string("'");
  if (name == "nbsp") return std::string(" ");
  if (name == "mdash") return std::string("—");
  if (name == "ndash") return std::string("–");
  if (name == "hellip") return std::string("…");
  if (name == "copy") return std::string("©");
  if (name == "reg") return std::string("®");
  if (name == "trade") return std::string("™");
  if (name == "rsquo") return std::string("’");
  if (name == "lsquo") return std::string("‘");
  if (name == "ldquo") return std::string("“");
  if (name == "rdquo") return std::string("”");
  return std::nullopt;
}

std::string decode_entities(std::string_view text) {
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '&') {
      out += text[i];
      continue;
    }

    const size_t semicolon = text.find(';', i + 1);
    // A stray '&' in prose is common; only a short, well-formed entity is
    // decoded, everything else passes through untouched.
    if (semicolon == std::string_view::npos or semicolon - i > 12) {
      out += text[i];
      continue;
    }

    const std::string_view body = text.substr(i + 1, semicolon - i - 1);
    if (body.empty()) {
      out += text[i];
      continue;
    }

    if (body[0] == '#') {
      const bool hex = body.size() > 1 and (body[1] == 'x' or body[1] == 'X');
      const std::string_view digits = body.substr(hex ? 2 : 1);
      if (digits.empty()) {
        out += text[i];
        continue;
      }

      uint32_t code_point = 0;
      bool valid = true;
      for (const char c : digits) {
        const int value = hex ? (std::isdigit(static_cast<unsigned char>(c))
                                     ? c - '0'
                                     : (std::isxdigit(static_cast<unsigned char>(c))
                                            ? std::tolower(c) - 'a' + 10
                                            : -1))
                              : (std::isdigit(static_cast<unsigned char>(c))
                                     ? c - '0'
                                     : -1);
        if (value < 0) {
          valid = false;
          break;
        }
        code_point = code_point * (hex ? 16 : 10) + static_cast<uint32_t>(value);
      }

      if (not valid or code_point == 0 or code_point > 0x10FFFF) {
        out += text[i];
        continue;
      }
      append_utf8(out, code_point);
      i = semicolon;
      continue;
    }

    if (const std::optional<std::string> decoded = named_entity(body)) {
      out += *decoded;
      i = semicolon;
      continue;
    }

    out += text[i];
  }

  return out;
}

// ---------------------------------------------------------------------------
// HTML -> markdown.
//
// A single forward scan, no DOM. Deliberately lenient: an unknown tag is
// dropped and its text kept, and unbalanced tags don't derail the rest of the
// document — real pages are rarely well-formed and a strict parser would
// return nothing far too often.
// ---------------------------------------------------------------------------

using Attributes = std::map<std::string, std::string>;

struct ListLevel {
  bool ordered = false;
  int item = 0;
};

struct HtmlConverter {
  explicit HtmlConverter(std::string base_url) : base_url_(std::move(base_url)) {}

  std::string convert(std::string_view html);

private:
  void handle_open(const std::string& tag, const Attributes& attributes);
  void handle_close(const std::string& tag);
  void emit_text(std::string_view text);

  // Where text lands: straight into the output, or into the pending link label
  // while an <a> is open.
  std::string& sink() { return in_link_ ? link_text_ : out_; }

  void append(std::string_view text) { sink() += text; }
  void ensure_newline();
  void ensure_block();
  void line_prefix();

  std::string base_url_;
  std::string out_;

  bool in_link_ = false;
  std::string link_text_;
  std::string link_href_;

  std::vector<ListLevel> lists_;
  int quote_depth_ = 0;
  int pre_depth_ = 0;
  bool row_has_cell_ = false;
};

void HtmlConverter::line_prefix() {
  for (int i = 0; i < quote_depth_; ++i) sink() += "> ";
}

void HtmlConverter::ensure_newline() {
  std::string& target = sink();
  if (target.empty()) return;
  if (target.back() == '\n') {
    line_prefix();
    return;
  }
  target += "\n";
  line_prefix();
}

void HtmlConverter::ensure_block() {
  std::string& target = sink();
  if (target.empty()) return;

  // Back off any prefix already written for an empty line, so a blank line
  // stays blank.
  while (not target.empty() and (target.back() == ' ' or target.back() == '>')) {
    target.pop_back();
  }
  if (target.empty()) return;

  size_t trailing = 0;
  for (auto it = target.rbegin(); it != target.rend() and *it == '\n'; ++it) {
    ++trailing;
  }
  for (size_t i = trailing; i < 2; ++i) target += "\n";
  line_prefix();
}

void HtmlConverter::emit_text(std::string_view text) {
  const std::string decoded = decode_entities(text);

  if (pre_depth_ > 0) {
    append(decoded);
    return;
  }

  // Outside <pre>, HTML whitespace is insignificant: runs collapse to one
  // space, and a run at the start of a line disappears entirely.
  std::string& target = sink();
  for (const char c : decoded) {
    if (not is_space(c)) {
      target += c;
      continue;
    }
    if (target.empty()) continue;
    if (target.back() == ' ' or target.back() == '\n') continue;
    target += ' ';
  }
}

void HtmlConverter::handle_open(const std::string& tag,
                                const Attributes& attributes) {
  if (tag.size() == 2 and tag[0] == 'h' and tag[1] >= '1' and tag[1] <= '6') {
    ensure_block();
    append(std::string(static_cast<size_t>(tag[1] - '0'), '#') + " ");
    return;
  }

  if (tag == "p" or tag == "div" or tag == "section" or tag == "article" or
      tag == "main" or tag == "header" or tag == "footer" or tag == "nav" or
      tag == "aside" or tag == "figure" or tag == "figcaption" or
      tag == "table" or tag == "form" or tag == "dl" or tag == "dt" or
      tag == "dd" or tag == "title") {
    ensure_block();
    return;
  }

  if (tag == "br") {
    ensure_newline();
    return;
  }

  if (tag == "hr") {
    ensure_block();
    append("---");
    ensure_block();
    return;
  }

  if (tag == "ul" or tag == "ol") {
    ensure_block();
    lists_.push_back({tag == "ol", 0});
    return;
  }

  if (tag == "li") {
    ensure_newline();
    if (lists_.empty()) {
      append("- ");
      return;
    }
    ListLevel& level = lists_.back();
    append(std::string(2 * (lists_.size() - 1), ' '));
    if (level.ordered) {
      append(std::to_string(++level.item) + ". ");
    } else {
      append("- ");
    }
    return;
  }

  if (tag == "blockquote") {
    ensure_block();
    ++quote_depth_;
    line_prefix();
    return;
  }

  if (tag == "pre") {
    ensure_block();
    append("```\n");
    ++pre_depth_;
    return;
  }

  if (tag == "code") {
    if (pre_depth_ == 0) append("`");
    return;
  }

  if (tag == "strong" or tag == "b") {
    append("**");
    return;
  }

  if (tag == "em" or tag == "i") {
    append("*");
    return;
  }

  if (tag == "tr") {
    ensure_newline();
    row_has_cell_ = false;
    return;
  }

  if (tag == "td" or tag == "th") {
    if (row_has_cell_) append(" | ");
    row_has_cell_ = true;
    return;
  }

  if (tag == "a") {
    const auto href = attributes.find("href");
    link_href_ =
        href == attributes.end() ? std::string() : resolve_url(base_url_, href->second);
    link_text_.clear();
    in_link_ = true;
    return;
  }
}

void HtmlConverter::handle_close(const std::string& tag) {
  if (tag == "a") {
    if (not in_link_) return;
    in_link_ = false;
    const std::string label = trim(link_text_);
    if (label.empty()) return;
    if (link_href_.empty()) {
      out_ += label;
    } else {
      out_ += "[" + label + "](" + link_href_ + ")";
    }
    return;
  }

  if (tag.size() == 2 and tag[0] == 'h' and tag[1] >= '1' and tag[1] <= '6') {
    ensure_block();
    return;
  }

  if (tag == "p" or tag == "div" or tag == "section" or tag == "article" or
      tag == "main" or tag == "header" or tag == "footer" or tag == "nav" or
      tag == "aside" or tag == "figure" or tag == "figcaption" or
      tag == "table" or tag == "form" or tag == "dl" or tag == "dt" or
      tag == "dd" or tag == "title") {
    ensure_block();
    return;
  }

  if (tag == "ul" or tag == "ol") {
    if (not lists_.empty()) lists_.pop_back();
    ensure_block();
    return;
  }

  if (tag == "blockquote") {
    if (quote_depth_ > 0) --quote_depth_;
    ensure_block();
    return;
  }

  if (tag == "pre") {
    if (pre_depth_ > 0) --pre_depth_;
    ensure_newline();
    append("```");
    ensure_block();
    return;
  }

  if (tag == "code") {
    if (pre_depth_ == 0) append("`");
    return;
  }

  if (tag == "strong" or tag == "b") {
    append("**");
    return;
  }

  if (tag == "em" or tag == "i") {
    append("*");
    return;
  }
}

std::string HtmlConverter::convert(std::string_view html) {
  // Non-empty while inside an element whose content is dropped wholesale;
  // holds the tag name to watch for on the way out.
  std::string skipping;

  size_t i = 0;
  while (i < html.size()) {
    const size_t next = html.find('<', i);
    if (next == std::string_view::npos) {
      if (skipping.empty()) emit_text(html.substr(i));
      break;
    }

    if (next > i and skipping.empty()) emit_text(html.substr(i, next - i));

    // Comments and doctypes carry nothing worth rendering.
    if (html.compare(next, 4, "<!--") == 0) {
      const size_t end = html.find("-->", next + 4);
      i = end == std::string_view::npos ? html.size() : end + 3;
      continue;
    }
    if (next + 1 < html.size() and html[next + 1] == '!') {
      const size_t end = html.find('>', next);
      i = end == std::string_view::npos ? html.size() : end + 1;
      continue;
    }

    const size_t tag_end = html.find('>', next);
    if (tag_end == std::string_view::npos) {
      if (skipping.empty()) emit_text(html.substr(next));
      break;
    }

    std::string_view inner = html.substr(next + 1, tag_end - next - 1);
    i = tag_end + 1;

    const bool closing = not inner.empty() and inner[0] == '/';
    if (closing) inner.remove_prefix(1);
    if (not inner.empty() and inner.back() == '/') inner.remove_suffix(1);

    size_t name_end = 0;
    while (name_end < inner.size() and not is_space(inner[name_end])) ++name_end;
    const std::string tag = to_lower(inner.substr(0, name_end));
    if (tag.empty()) continue;

    if (not skipping.empty()) {
      if (closing and tag == skipping) skipping.clear();
      continue;
    }

    if (not closing and (tag == "script" or tag == "style" or
                         tag == "noscript" or tag == "svg" or
                         tag == "template" or tag == "iframe" or
                         tag == "canvas")) {
      skipping = tag;
      continue;
    }

    if (closing) {
      handle_close(tag);
      continue;
    }

    // Attributes are only ever needed for <a href>, so parsing them for every
    // tag would be wasted work on attribute-heavy pages.
    Attributes attributes;
    if (tag == "a") {
      std::string_view rest = inner.substr(name_end);
      size_t j = 0;
      while (j < rest.size()) {
        while (j < rest.size() and is_space(rest[j])) ++j;
        const size_t key_start = j;
        while (j < rest.size() and not is_space(rest[j]) and rest[j] != '=') ++j;
        if (j == key_start) break;

        const std::string key = to_lower(rest.substr(key_start, j - key_start));
        while (j < rest.size() and is_space(rest[j])) ++j;

        std::string value;
        if (j < rest.size() and rest[j] == '=') {
          ++j;
          while (j < rest.size() and is_space(rest[j])) ++j;
          if (j < rest.size() and (rest[j] == '"' or rest[j] == '\'')) {
            const char quote = rest[j++];
            const size_t value_start = j;
            while (j < rest.size() and rest[j] != quote) ++j;
            value = decode_entities(rest.substr(value_start, j - value_start));
            if (j < rest.size()) ++j;
          } else {
            const size_t value_start = j;
            while (j < rest.size() and not is_space(rest[j])) ++j;
            value = decode_entities(rest.substr(value_start, j - value_start));
          }
        }
        attributes.emplace(key, std::move(value));
      }
    }

    handle_open(tag, attributes);
  }

  // An unclosed <a> would otherwise swallow the rest of the page.
  if (in_link_) handle_close("a");

  // Collapse the runs of blank lines that block tags inevitably produce, and
  // drop the trailing whitespace on each line.
  std::string cleaned;
  cleaned.reserve(out_.size());
  size_t blank_run = 0;
  size_t line_start = 0;
  for (size_t j = 0; j <= out_.size(); ++j) {
    if (j < out_.size() and out_[j] != '\n') continue;

    std::string_view line(out_.data() + line_start, j - line_start);
    while (not line.empty() and (line.back() == ' ' or line.back() == '\t')) {
      line.remove_suffix(1);
    }
    line_start = j + 1;

    if (line.empty() or line == ">") {
      ++blank_run;
      continue;
    }
    // Any run of blank lines becomes exactly one, so blocks stay separated the
    // way markdown expects without the pile-up nested block tags produce.
    if (not cleaned.empty()) cleaned += blank_run > 0 ? "\n\n" : "\n";
    blank_run = 0;
    cleaned += line;
  }

  return cleaned;
}

// ---------------------------------------------------------------------------
// Content type dispatch.
// ---------------------------------------------------------------------------

enum struct Rendering { Html, Text, Unsupported };

Rendering rendering_for(const std::string& content_type, const std::string& body) {
  // "text/html; charset=utf-8" -> "text/html"
  std::string media = to_lower(content_type);
  const size_t semicolon = media.find(';');
  if (semicolon != std::string::npos) media.resize(semicolon);
  media = trim(media);

  if (media.empty()) {
    // No header to go on: anything with a NUL byte is binary, the rest is
    // probably readable.
    return is_binary(std::string_view(body).substr(
               0, std::min<size_t>(body.size(), 8192)))
               ? Rendering::Unsupported
               : Rendering::Text;
  }

  if (media == "text/html" or media == "application/xhtml+xml") {
    return Rendering::Html;
  }
  if (media.rfind("text/", 0) == 0) return Rendering::Text;
  if (media == "application/json" or media == "application/xml" or
      media == "application/javascript" or media == "application/x-ndjson") {
    return Rendering::Text;
  }
  if (media.size() > 5 and
      (media.compare(media.size() - 5, 5, "+json") == 0 or
       media.compare(media.size() - 4, 4, "+xml") == 0)) {
    return Rendering::Text;
  }

  return Rendering::Unsupported;
}

}  // namespace

// ---------------------------------------------------------------------------

std::string WebFetchTool::description() const {
  return R"json({"name":"webfetch","description":"Fetch a website and return its content.","parameters":{"type":"object","properties":{"url":{"type":"string","description":"URL of the website to fetch"}},"required":["url"]}})json";
}

ToolResult WebFetchTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> url = string_arg(args, "url");
  if (not url or url->empty()) {
    result.error = "webfetch: missing required string argument 'url'";
    return result;
  }

  ensure_curl_initialized();

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    result.error = "webfetch: failed to initialize curl handle";
    return result;
  }

  DownloadSink sink;
  curl_easy_setopt(curl, CURLOPT_URL, url->c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

  const CURLcode code = curl_easy_perform(curl);

  // CURLE_WRITE_ERROR is how the size cap reports itself, and what arrived
  // before the cut is still worth returning.
  if (code != CURLE_OK and not(code == CURLE_WRITE_ERROR and sink.capped)) {
    result.error = "webfetch: " + std::string(curl_easy_strerror(code));
    curl_easy_cleanup(curl);
    return result;
  }

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  const char* effective_url = nullptr;
  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
  const std::string final_url =
      effective_url != nullptr ? std::string(effective_url) : *url;

  const char* content_type = nullptr;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
  const std::string media_type =
      content_type != nullptr ? std::string(content_type) : std::string();

  curl_easy_cleanup(curl);

  // A non-2xx body is rarely what was wanted, so it's an error rather than
  // content — but the status is named so the model can react to it.
  if (status != 0 and (status < 200 or status >= 300)) {
    result.error = "webfetch: " + final_url + " returned HTTP " +
                   std::to_string(status);
    return result;
  }

  const Rendering rendering = rendering_for(media_type, sink.data);
  if (rendering == Rendering::Unsupported) {
    result.error = "webfetch: " + final_url + " returned unsupported content" +
                   (media_type.empty() ? " (binary)" : " type " + media_type);
    return result;
  }

  std::string content = rendering == Rendering::Html
                            ? HtmlConverter(final_url).convert(sink.data)
                            : sink.data;

  TruncatedOutput truncated = truncate_output(std::move(content), "webfetch");

  result.ok = true;
  result.output = "[fetched " + final_url +
                  (media_type.empty() ? "" : " (" + media_type + ")") + "]\n";
  result.output += truncated.text;
  result.output += truncation_note(truncated);
  if (sink.capped) {
    result.output += "\n[download stopped at " +
                     std::to_string(kMaxDownloadBytes / (1024 * 1024)) + "MB]";
  }
  result.truncated = truncated.truncated or sink.capped;
  result.overflow_path = std::move(truncated.overflow_path);
  return result;
}

}  // namespace agent
