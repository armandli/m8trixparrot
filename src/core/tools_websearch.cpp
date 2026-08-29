#include <core/tools.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <curl/curl.h>
#include <simdjson.h>

#include <core/json_util.h>
#include <core/tools_util.h>

namespace agent {

namespace {

// The schema names no default, so pick one; the request also passes it to
// Parallel as advanced_settings.max_results.
constexpr int64_t kDefaultResultLimit = 10;

// Parallel's search endpoint. PARALLEL_API_BASE overrides the host — the seam
// the offline unit test points at a loopback server, and a hook for a proxy.
constexpr const char* kDefaultApiBase = "https://api.parallel.ai";
constexpr const char* kSearchPath = "/v1/search";

// The key file, relative to the working directory — a sibling of
// .m8trix/settings.json and .m8trix/sessions. Gitignored by the .m8trix/* rule.
constexpr const char* kApiKeyFile = ".m8trix/parallel_api_key";

constexpr long kTimeoutSeconds = 30;
constexpr long kConnectTimeoutSeconds = 10;

// Each result's snippet is clipped here so one verbose hit can't crowd out the
// rest; truncate_output() still caps the whole thing.
constexpr size_t kMaxExcerptChars = 600;

// curl_global_init() must run once before any handle is used and is not itself
// thread-safe, so it happens through a function-local static — the same idiom
// as tools_web.cpp and ollama_client.cpp.
void ensure_curl_initialized() {
  static const CURLcode init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  (void)init_result;
}

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const size_t bytes = size * nmemb;
  out->append(ptr, bytes);
  return bytes;
}

std::string trim(std::string_view text) {
  const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end and is_space(text[begin])) ++begin;
  while (end > begin and is_space(text[end - 1])) --end;
  return std::string(text.substr(begin, end - begin));
}

std::string clip(std::string text, size_t max_chars) {
  if (text.size() <= max_chars) return text;
  return text.substr(0, max_chars) + "...";
}

// Parallel's excerpts are markdown and often span several lines; a search
// listing reads better with each snippet flattened to a single line, runs of
// whitespace collapsed to one space.
std::string flatten(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pending_space = false;
  for (const unsigned char c : text) {
    if (std::isspace(c) != 0) {
      pending_space = not out.empty();
      continue;
    }
    if (pending_space) out += ' ';
    pending_space = false;
    out += static_cast<char>(c);
  }
  return out;
}

// PARALLEL_API_KEY, or the trimmed contents of .m8trix/parallel_api_key.
std::optional<std::string> resolve_api_key() {
  if (const char* env = std::getenv("PARALLEL_API_KEY");
      env != nullptr and *env != '\0') {
    return std::string(env);
  }
  if (const std::optional<std::string> file = read_file(kApiKeyFile)) {
    std::string key = trim(*file);
    if (not key.empty()) return key;
  }
  return std::nullopt;
}

std::string search_url() {
  const char* base = std::getenv("PARALLEL_API_BASE");
  const std::string host =
      (base != nullptr and *base != '\0') ? std::string(base) : kDefaultApiBase;
  return host + kSearchPath;
}

// Parallel reports a bad key as {"code":..,"message":..} and a bad request as
// {"type":"error","error":{"message":..}}. Best effort: returns "" when the
// body is neither shape.
std::string api_error_detail(const std::string& body) {
  try {
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(body);
    simdjson::ondemand::document doc = parser.iterate(padded);

    simdjson::ondemand::object obj;
    if (doc.get_object().get(obj)) return std::string();

    if (simdjson::ondemand::object error;
        not obj["error"].get_object().get(error)) {
      const std::string message = string_field(error, "message");
      if (not message.empty()) return ": " + message;
    }

    const std::string message = string_field(obj, "message");
    return message.empty() ? std::string() : ": " + message;
  } catch (const std::exception&) {
    return std::string();
  }
}

// Turns a 200 body into the numbered listing the model sees. Returns a
// non-empty error string when the body isn't a usable search response.
std::string format_response(const std::string& body, const std::string& query,
                            int64_t limit, std::string& out) {
  try {
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(body);
    simdjson::ondemand::document doc = parser.iterate(padded);

    simdjson::ondemand::object obj;
    if (doc.get_object().get(obj)) {
      return "Parallel response was not a JSON object";
    }

    simdjson::ondemand::array results;
    if (obj["results"].get_array().get(results)) {
      return "Parallel response had no 'results' array";
    }

    std::string entries;
    int64_t shown = 0;
    for (auto element : results) {
      if (shown >= limit) break;

      simdjson::ondemand::object result;
      if (element.get_object().get(result)) continue;

      const std::string url = string_field(result, "url");
      const std::string title = string_field(result, "title");
      const std::string date = string_field(result, "publish_date");

      std::string excerpt;
      simdjson::ondemand::array excerpts;
      if (not result["excerpts"].get_array().get(excerpts)) {
        for (auto item : excerpts) {
          std::string_view piece;
          if (item.get_string().get(piece)) continue;
          const std::string flat = flatten(piece);
          if (flat.empty()) continue;
          if (not excerpt.empty()) excerpt += " … ";
          excerpt += flat;
        }
      }

      if (url.empty() and title.empty()) continue;
      ++shown;

      const std::string heading = title.empty() ? url : title;
      entries += std::to_string(shown) + ". " + heading + "\n";
      if (not url.empty()) {
        entries += "   " + url;
        if (not date.empty()) entries += "  (" + date + ")";
        entries += "\n";
      }
      if (not excerpt.empty()) {
        entries += "   " + clip(std::move(excerpt), kMaxExcerptChars) + "\n";
      }
      entries += "\n";
    }

    if (shown == 0) {
      out = "[no results for \"" + query + "\"]";
      return std::string();
    }

    out = std::to_string(shown) +
          (shown == 1 ? " result for \"" : " results for \"") + query +
          "\":\n\n" + entries;
    while (not out.empty() and out.back() == '\n') out.pop_back();
    return std::string();
  } catch (const std::exception& e) {
    return std::string("could not parse Parallel response: ") + e.what();
  }
}

}  // namespace

bool web_search_available() { return resolve_api_key().has_value(); }

std::string WebSearchTool::description() const {
  return R"json({"name":"websearch","description":"Search the web for a query and get back a numbered list of results, each with a title, URL, and a short snippet. URLs are kept intact so webfetch can follow them.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Search query string"},"limit":{"type":"number","description":"Maximum number of results to return (default 10)"}},"required":["query"]}})json";
}

ToolResult WebSearchTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> query = string_arg(args, "query");
  if (not query or query->empty()) {
    result.error = "websearch: missing required string argument 'query'";
    return result;
  }

  const int64_t limit =
      std::max<int64_t>(int_arg(args, "limit").value_or(kDefaultResultLimit), 1);

  const std::optional<std::string> api_key = resolve_api_key();
  if (not api_key) {
    result.error =
        "websearch: no Parallel API key; set PARALLEL_API_KEY or write the key "
        "to .m8trix/parallel_api_key";
    return result;
  }

  JsonWriter body;
  body.begin_object();
  body.field("objective", *query);
  body.key("search_queries").begin_array().value(*query).end_array();
  body.key("advanced_settings")
      .begin_object()
      .field("max_results", limit)
      .end_object();
  body.end_object();
  const std::string payload = body.str();

  ensure_curl_initialized();
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    result.error = "websearch: failed to initialize curl handle";
    return result;
  }

  const std::string url = search_url();
  std::string response;

  curl_slist* headers =
      curl_slist_append(nullptr, "Content-Type: application/json");
  const std::string key_header = "x-api-key: " + *api_key;
  headers = curl_slist_append(headers, key_header.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "m8trixparrot/1.0");

  const CURLcode code = curl_easy_perform(curl);
  curl_slist_free_all(headers);

  if (code != CURLE_OK) {
    result.error = "websearch: " + std::string(curl_easy_strerror(code));
    curl_easy_cleanup(curl);
    return result;
  }

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);

  if (status < 200 or status >= 300) {
    result.error = "websearch: Parallel API returned HTTP " +
                   std::to_string(status) + api_error_detail(response);
    return result;
  }

  std::string formatted;
  const std::string parse_error =
      format_response(response, *query, limit, formatted);
  if (not parse_error.empty()) {
    result.error = "websearch: " + parse_error;
    return result;
  }

  TruncatedOutput truncated = truncate_output(std::move(formatted), "websearch");
  result.ok = true;
  result.output = std::move(truncated.text);
  result.output += truncation_note(truncated);
  result.truncated = truncated.truncated;
  result.overflow_path = std::move(truncated.overflow_path);
  return result;
}

}  // namespace agent
