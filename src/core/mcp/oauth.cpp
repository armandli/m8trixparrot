#include <core/mcp/oauth.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>

#include <core/mcp/protocol.h>
#include <core/util/base64.h>
#include <core/util/secure_random.h>
#include <core/util/sha256.h>
#include <core/util/url.h>

namespace mcp {

namespace {

using util::JsonValue;

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::vector<std::string> string_list(const JsonValue& value) {
  std::vector<std::string> out;
  for (const JsonValue& item : value.items()) {
    if (item.is_string()) out.push_back(item.as_string());
  }
  return out;
}

std::vector<std::string> split_scopes(std::string_view text) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() and text[at] == ' ') ++at;
    const size_t end = std::min(text.find(' ', at), text.size());
    if (end > at) out.emplace_back(text.substr(at, end - at));
    at = end;
  }
  return out;
}

std::string join_scopes(const std::vector<std::string>& scopes) {
  std::string out;
  for (const std::string& scope : scopes) {
    if (not out.empty()) out += ' ';
    out += scope;
  }
  return out;
}

// Scopes in first-seen order, without repeats.
void add_scopes(std::vector<std::string>& into, std::string_view text) {
  for (const std::string& scope : split_scopes(text)) {
    if (std::find(into.begin(), into.end(), scope) == into.end()) into.push_back(scope);
  }
}

// The path of a URL without its trailing slash ("" for the root).
std::string bare_path(const util::Url& url) {
  std::string path = url.path;
  while (not path.empty() and path.back() == '/') path.pop_back();
  return path;
}

// ── HTTP for the OAuth endpoints: small requests, no redirects ──

void ensure_curl_initialized() {
  static const CURLcode init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  (void)init_result;
}

struct HttpResult {
  long status = 0;
  std::string body;
  std::string error;  // a transport failure
};

size_t collect(char* data, size_t size, size_t count, void* user) {
  auto* body = static_cast<std::string*>(user);
  const size_t bytes = size * count;
  if (body->size() + bytes > (1u << 20)) return 0;  // nothing here is that big
  body->append(data, bytes);
  return bytes;
}

HttpResult http_call(const std::string& method, const std::string& url,
                     const std::vector<std::string>& headers, const std::string& body,
                     std::chrono::milliseconds timeout) {
  ensure_curl_initialized();
  HttpResult result;
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    result.error = "could not set up an HTTP request";
    return result;
  }
  char error[CURL_ERROR_SIZE] = {};
  curl_slist* list = nullptr;
  for (const std::string& header : headers) list = curl_slist_append(list, header.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
  curl_easy_setopt(curl, CURLOPT_USERAGENT, (std::string(kClientName) + "/" + kClientVersion).c_str());
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  }
  const CURLcode code = curl_easy_perform(curl);
  if (code == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
  } else {
    result.error = error[0] != '\0' ? std::string(error) : curl_easy_strerror(code);
  }
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return result;
}

std::optional<JsonValue> fetch_json(const std::string& url, std::chrono::milliseconds timeout,
                                    std::string& error) {
  if (not secure_endpoint(url)) {
    error = url + " is neither https nor on this machine";
    return std::nullopt;
  }
  const HttpResult result = http_call("GET", url, {"Accept: application/json"}, "", timeout);
  if (not result.error.empty()) {
    error = result.error;
    return std::nullopt;
  }
  if (result.status != 200) {
    error = url + ": HTTP " + std::to_string(result.status);
    return std::nullopt;
  }
  std::optional<JsonValue> document = JsonValue::parse(result.body);
  if (not document or not document->is_object()) {
    error = url + " did not return a JSON object";
    return std::nullopt;
  }
  return document;
}

// "invalid_grant: the code expired" from an OAuth error response.
std::string oauth_error(const HttpResult& result) {
  const std::optional<JsonValue> document = JsonValue::parse(result.body);
  std::string text = "HTTP " + std::to_string(result.status);
  if (document and document->get("error").is_string()) {
    text = document->get("error").as_string();
    if (document->get("error_description").is_string()) {
      text += ": " + document->get("error_description").as_string();
    }
  }
  return text;
}

}  // namespace

// ───────────────────────────── challenges ───────────────────────────────────

std::vector<AuthChallenge> parse_www_authenticate(std::string_view header) {
  std::vector<AuthChallenge> out;
  const auto tchar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) or
           std::strchr("!#$%&'*+-.^_`|~", c) != nullptr;
  };
  size_t i = 0;
  const size_t n = header.size();
  const auto skip = [&](bool commas) {
    while (i < n and (header[i] == ' ' or header[i] == '\t' or (commas and header[i] == ','))) ++i;
  };
  while (i < n) {
    skip(true);
    const size_t start = i;
    while (i < n and tchar(header[i])) ++i;
    if (i == start) {
      ++i;  // something unparseable: step over it
      continue;
    }
    const std::string token(header.substr(start, i - start));
    skip(false);
    // `name=value` belongs to the challenge before it; `name ==` would be a
    // token68, which no MCP challenge uses.
    if (i < n and header[i] == '=' and (i + 1 >= n or header[i + 1] != '=') and not out.empty()) {
      ++i;
      skip(false);
      std::string value;
      if (i < n and header[i] == '"') {
        ++i;
        while (i < n and header[i] != '"') {
          if (header[i] == '\\' and i + 1 < n) ++i;
          value += header[i++];
        }
        if (i < n) ++i;  // the closing quote
      } else {
        while (i < n and header[i] != ',' and header[i] != ' ' and header[i] != '\t') {
          value += header[i++];
        }
      }
      out.back().params[lower(token)] = value;
      continue;
    }
    AuthChallenge challenge;
    challenge.scheme = lower(token);
    out.push_back(std::move(challenge));
  }
  return out;
}

std::string bearer_param(std::string_view header, const std::string& name) {
  for (const AuthChallenge& challenge : parse_www_authenticate(header)) {
    if (challenge.scheme != "bearer") continue;
    if (auto it = challenge.params.find(lower(name)); it != challenge.params.end()) {
      return it->second;
    }
  }
  return std::string();
}

bool insufficient_scope(std::string_view www_authenticate) {
  return bearer_param(www_authenticate, "error") == "insufficient_scope";
}

// ───────────────────────────── discovery ────────────────────────────────────

std::string canonical_resource(const std::string& server_url) {
  const std::optional<util::Url> url = util::parse_url(server_url);
  if (not url) return server_url;
  std::string out = util::url_origin(*url) + bare_path(*url);
  if (not url->query.empty()) out += "?" + url->query;
  return out;
}

std::vector<std::string> resource_metadata_urls(const std::string& server_url,
                                                const std::string& from_challenge) {
  std::vector<std::string> urls;
  if (not from_challenge.empty()) urls.push_back(from_challenge);
  const std::optional<util::Url> url = util::parse_url(server_url);
  if (not url) return urls;
  const std::string origin = util::url_origin(*url);
  const std::string path = bare_path(*url);
  if (not path.empty()) urls.push_back(origin + "/.well-known/oauth-protected-resource" + path);
  urls.push_back(origin + "/.well-known/oauth-protected-resource");
  return urls;
}

std::vector<std::string> authorization_server_metadata_urls(const std::string& issuer) {
  const std::optional<util::Url> url = util::parse_url(issuer);
  if (not url) return {};
  const std::string origin = util::url_origin(*url);
  const std::string path = bare_path(*url);
  if (path.empty()) {
    return {origin + "/.well-known/oauth-authorization-server",
            origin + "/.well-known/openid-configuration"};
  }
  return {origin + "/.well-known/oauth-authorization-server" + path,
          origin + "/.well-known/openid-configuration" + path,
          origin + path + "/.well-known/openid-configuration"};
}

bool secure_endpoint(const std::string& text) {
  const std::optional<util::Url> url = util::parse_url(text);
  if (not url) return false;
  return url->scheme == "https" or (url->scheme == "http" and util::is_loopback_host(url->host));
}

std::optional<ResourceMetadata> parse_resource_metadata(const JsonValue& document,
                                                        const std::string& server_url,
                                                        std::string& error) {
  ResourceMetadata metadata;
  metadata.resource = document.get("resource").as_string();
  metadata.authorization_servers = string_list(document.get("authorization_servers"));
  metadata.scopes_supported = string_list(document.get("scopes_supported"));
  if (metadata.authorization_servers.empty()) {
    error = "the protected resource metadata names no authorization server";
    return std::nullopt;
  }
  const std::optional<util::Url> resource = util::parse_url(metadata.resource);
  const std::optional<util::Url> server = util::parse_url(server_url);
  if (not resource or not server) {
    error = "the protected resource metadata has no usable \"resource\"";
    return std::nullopt;
  }
  // The metadata must be about this server: same origin, and its resource
  // the server URL or one of its ancestors.
  const std::string resource_path = bare_path(*resource);
  const std::string server_path = bare_path(*server);
  const bool same_origin = util::url_origin(*resource) == util::url_origin(*server);
  const bool covers = resource_path.empty() or server_path == resource_path or
                      server_path.rfind(resource_path + "/", 0) == 0;
  if (not same_origin or not covers) {
    error = "the protected resource metadata is for " + metadata.resource + ", not " + server_url;
    return std::nullopt;
  }
  return metadata;
}

std::optional<AuthServerMetadata> parse_auth_server_metadata(const JsonValue& document,
                                                             const std::string& expected_issuer,
                                                             std::string& error) {
  AuthServerMetadata metadata;
  metadata.issuer = document.get("issuer").as_string();
  // RFC 8414 section 3.3: anything else is metadata about some other server.
  if (metadata.issuer != expected_issuer) {
    error = "the metadata names issuer '" + metadata.issuer + "', not '" + expected_issuer + "'";
    return std::nullopt;
  }
  metadata.authorization_endpoint = document.get("authorization_endpoint").as_string();
  metadata.token_endpoint = document.get("token_endpoint").as_string();
  metadata.registration_endpoint = document.get("registration_endpoint").as_string();
  metadata.scopes_supported = string_list(document.get("scopes_supported"));
  metadata.token_endpoint_auth_methods =
      string_list(document.get("token_endpoint_auth_methods_supported"));
  metadata.client_id_metadata_document_supported =
      document.get("client_id_metadata_document_supported").as_bool();
  metadata.iss_parameter_supported =
      document.get("authorization_response_iss_parameter_supported").as_bool();

  const std::vector<std::string> methods = string_list(document.get("code_challenge_methods_supported"));
  if (std::find(methods.begin(), methods.end(), "S256") == methods.end()) {
    error = "the authorization server does not support PKCE with S256, which m8 requires";
    return std::nullopt;
  }
  if (metadata.authorization_endpoint.empty() or metadata.token_endpoint.empty()) {
    error = "the authorization server's metadata lacks its endpoints";
    return std::nullopt;
  }
  for (const std::string* endpoint : {&metadata.authorization_endpoint, &metadata.token_endpoint,
                                      &metadata.registration_endpoint}) {
    if (not endpoint->empty() and not secure_endpoint(*endpoint)) {
      error = "the authorization server's endpoint " + *endpoint + " is not https";
      return std::nullopt;
    }
  }
  return metadata;
}

std::string check_issuer(bool iss_parameter_supported, const std::optional<std::string>& iss,
                         const std::string& expected_issuer) {
  if (iss) {
    // Simple string comparison, no normalization (RFC 9207 section 2.4).
    if (*iss != expected_issuer) {
      return "the browser came back from issuer '" + *iss + "', not '" + expected_issuer +
             "' (a mix-up attack, or a misconfigured server); ignored";
    }
    return std::string();
  }
  if (iss_parameter_supported) {
    return "the authorization server promised to say who it is (iss) and did not; ignored";
  }
  return std::string();
}

std::optional<TokenSet> parse_token_response(const JsonValue& document, int64_t now,
                                             std::string& error) {
  TokenSet tokens;
  tokens.access_token = document.get("access_token").as_string();
  if (tokens.access_token.empty()) {
    error = "the token response has no access_token";
    return std::nullopt;
  }
  const std::string type = lower(document.get("token_type").string_or("bearer"));
  if (type != "bearer") {
    error = "the authorization server issued a '" + type + "' token; m8 uses Bearer tokens";
    return std::nullopt;
  }
  tokens.refresh_token = document.get("refresh_token").as_string();
  tokens.scope = document.get("scope").as_string();
  const JsonValue& expires_in = document.get("expires_in");
  if (expires_in.is_number() and expires_in.as_int() > 0) {
    tokens.expires_at = now + expires_in.as_int();
  }
  return tokens;
}

std::string choose_scope(const std::string& challenge_scope, const std::string& previous,
                         const std::vector<std::string>& resource_scopes,
                         const std::vector<std::string>& server_scopes) {
  std::vector<std::string> scopes;
  if (not challenge_scope.empty()) {
    // A step-up keeps what was granted before (scope union).
    add_scopes(scopes, previous);
    add_scopes(scopes, challenge_scope);
  } else if (not previous.empty()) {
    add_scopes(scopes, previous);
  } else {
    for (const std::string& scope : resource_scopes) add_scopes(scopes, scope);
  }
  // Ask for refresh tokens where they are offered — but never turn "no scope
  // parameter" (the server's defaults) into "offline_access only".
  if (not scopes.empty() and
      std::find(server_scopes.begin(), server_scopes.end(), "offline_access") != server_scopes.end()) {
    add_scopes(scopes, "offline_access");
  }
  return join_scopes(scopes);
}

// ──────────────────────────── the listener ──────────────────────────────────

// One-request HTTP server on 127.0.0.1 for the browser's redirect.
class LoopbackListener {
public:
  ~LoopbackListener() {
    for (int* fd : {&mFd, &mWake[0], &mWake[1]}) {
      if (*fd >= 0) ::close(*fd);
      *fd = -1;
    }
  }

  bool start(int port, std::string& error) {
    mFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (mFd < 0) {
      error = std::string("could not open a socket: ") + std::strerror(errno);
      return false;
    }
    ::fcntl(mFd, F_SETFD, FD_CLOEXEC);
    const int yes = 1;
    ::setsockopt(mFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(mFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 or
        ::listen(mFd, 8) != 0) {
      error = "could not listen on 127.0.0.1:" + std::to_string(port) + ": " + std::strerror(errno);
      return false;
    }
    socklen_t length = sizeof(address);
    ::getsockname(mFd, reinterpret_cast<sockaddr*>(&address), &length);
    mPort = ntohs(address.sin_port);
    if (::pipe(mWake) != 0) {
      error = std::string("could not make a pipe: ") + std::strerror(errno);
      return false;
    }
    for (const int fd : mWake) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
      ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    return true;
  }

  int port() const { return mPort; }

  void wake() {
    if (mWake[1] >= 0) {
      const char byte = 'x';
      [[maybe_unused]] const ssize_t written = ::write(mWake[1], &byte, 1);
    }
  }

  // The query of the first GET /callback, answered with a page telling the
  // person to go back to m8. nullopt on cancel or the deadline.
  std::optional<std::vector<std::pair<std::string, std::string>>> wait(
      std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& cancelled,
      const std::atomic<bool>* also_cancelled) {
    while (true) {
      if (cancelled.load() or (also_cancelled != nullptr and also_cancelled->load())) {
        return std::nullopt;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) return std::nullopt;
      pollfd fds[2] = {{mFd, POLLIN, 0}, {mWake[0], POLLIN, 0}};
      ::poll(fds, 2, static_cast<int>(std::min<int64_t>(left.count(), 200)));
      if (fds[1].revents != 0) {
        char drain[16];
        while (::read(mWake[0], drain, sizeof(drain)) > 0) {
        }
        continue;
      }
      if ((fds[0].revents & POLLIN) == 0) continue;
      const int client = ::accept(mFd, nullptr, nullptr);
      if (client < 0) continue;
      ::fcntl(client, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
      ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes_, sizeof(yes_));
#endif
      std::string request;
      char chunk[2048];
      while (request.find("\r\n\r\n") == std::string::npos and request.size() < 16384) {
        pollfd readable{client, POLLIN, 0};
        if (::poll(&readable, 1, 2000) <= 0) break;
        const ssize_t got = ::recv(client, chunk, sizeof(chunk), 0);
        if (got <= 0) break;
        request.append(chunk, static_cast<size_t>(got));
      }
      const size_t line_end = request.find("\r\n");
      const std::string line = request.substr(0, line_end);
      const size_t first = line.find(' ');
      const size_t second = line.find(' ', first + 1);
      const std::string method = line.substr(0, first);
      const std::string target =
          first == std::string::npos ? std::string() : line.substr(first + 1, second - first - 1);
      const size_t question = target.find('?');
      const std::string path = target.substr(0, question);
      if (method != "GET" or path != "/callback") {
        respond(client, "404 Not Found", "Not here.");
        ::close(client);
        continue;
      }
      respond(client, "200 OK",
              "m8 has the answer from the authorization server. You can close this tab and "
              "go back to m8.");
      ::close(client);
      return util::form_decode(question == std::string::npos ? std::string_view()
                                                             : std::string_view(target).substr(question + 1));
    }
  }

private:
  static void respond(int client, const std::string& status, const std::string& text) {
    const std::string body = "<!doctype html><html><head><meta charset=\"utf-8\"><title>m8</title>"
                             "</head><body style=\"font-family:sans-serif\"><p>" +
                             text + "</p></body></html>";
    const std::string reply = "HTTP/1.1 " + status +
                              "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                              std::to_string(body.size()) +
                              "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
#if defined(MSG_NOSIGNAL)
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    size_t sent = 0;
    while (sent < reply.size()) {
      const ssize_t n = ::send(client, reply.data() + sent, reply.size() - sent, kFlags);
      if (n <= 0) return;
      sent += static_cast<size_t>(n);
    }
  }

  static constexpr int yes_ = 1;
  int mFd = -1;
  int mPort = 0;
  int mWake[2] = {-1, -1};
};

// ──────────────────────────────── login ─────────────────────────────────────

PendingLogin::~PendingLogin() = default;

int PendingLogin::port() const { return mListener ? mListener->port() : 0; }

void PendingLogin::cancel() {
  mCancelled.store(true);
  if (mListener) mListener->wake();
}

bool PendingLogin::wait(std::string& error, const std::atomic<bool>* also_cancelled) {
  const auto deadline = std::chrono::steady_clock::now() + mSession->mOptions.login_timeout;
  const auto params = mListener->wait(deadline, mCancelled, also_cancelled);
  if (not params) {
    error = mCancelled.load() or (also_cancelled != nullptr and also_cancelled->load())
                ? "the login was cancelled"
                : "the browser did not come back in time";
    return false;
  }
  std::string code;
  std::string state;
  std::optional<std::string> iss;
  std::string failure;
  std::string description;
  for (const auto& [name, value] : *params) {
    if (name == "code") code = value;
    if (name == "state") state = value;
    if (name == "iss") iss = value;
    if (name == "error") failure = value;
    if (name == "error_description") description = value;
  }
  // The issuer first: a response from another server is not acted on at all,
  // not even its error (RFC 9207, and the spec's table).
  if (const std::string problem = check_issuer(mIssSupported, iss, mIssuer); not problem.empty()) {
    error = problem;
    return false;
  }
  if (state != mState) {
    error = "the browser came back with a different state; not this login, ignored";
    return false;
  }
  if (not failure.empty()) {
    error = "the authorization server said: " + failure +
            (description.empty() ? std::string() : " (" + description + ")");
    return false;
  }
  if (code.empty()) {
    error = "the browser came back without an authorization code";
    return false;
  }
  return mSession->finish(*this, code, error);
}

OAuthSession::OAuthSession(OAuthOptions options)
    : mOptions(std::move(options)),
      mStore(mOptions.credentials_path),
      mKey(canonical_resource(mOptions.server_url)) {}

void OAuthSession::reload_locked() {
  std::error_code ec;
  const auto written = std::filesystem::last_write_time(mOptions.credentials_path, ec);
  const int64_t stamp = ec ? 0 : static_cast<int64_t>(written.time_since_epoch().count());
  if (mLoaded and stamp == mStoreStamp) return;
  mToken = mStore.token(mKey);
  mStoreStamp = stamp;
  mLoaded = true;
}

std::string OAuthSession::authorization() {
  if (mOptions.credentials_path.empty()) return std::string();
  std::lock_guard<std::mutex> lock(mMutex);
  reload_locked();
  if (not mToken) return std::string();
  const int64_t now = unix_now();
  if (mToken->expires_at > 0 and now + mOptions.refresh_margin.count() >= mToken->expires_at and
      not mToken->refresh_token.empty()) {
    std::string error;
    refresh_locked(error);  // failing, the old token goes, and the server's 401 says
  }
  return mToken ? "Bearer " + mToken->access_token : std::string();
}

bool OAuthSession::renew_after_rejection() {
  if (mOptions.credentials_path.empty()) return false;
  std::lock_guard<std::mutex> lock(mMutex);
  const std::string sent = mToken ? mToken->access_token : std::string();
  mLoaded = false;  // another m8 may have stored a newer one
  reload_locked();
  if (mToken and mToken->access_token != sent) return true;
  if (not mToken or mToken->refresh_token.empty()) return false;
  std::string error;
  return refresh_locked(error);
}

bool OAuthSession::refresh_locked(std::string& error) {
  bool renewed = false;
  const std::string before = mToken ? mToken->refresh_token : std::string();
  const bool locked = mStore.exclusive(
      [&] {
        // Under the file lock: another m8 may have refreshed it a moment ago.
        std::optional<StoredToken> current = mStore.token_unlocked(mKey);
        if (current and current->refresh_token != before and not current->access_token.empty()) {
          mToken = current;
          renewed = true;
          return;
        }
        if (not current or current->refresh_token.empty() or current->token_endpoint.empty()) return;

        std::vector<std::pair<std::string, std::string>> form = {
            {"grant_type", "refresh_token"},
            {"refresh_token", current->refresh_token},
            {"client_id", current->client_id},
            {"resource", current->resource}};
        std::vector<std::string> headers = {"Content-Type: application/x-www-form-urlencoded",
                                            "Accept: application/json"};
        if (not mOptions.settings.client_secret.empty() and
            current->client_id == mOptions.settings.client_id) {
          form.emplace_back("client_secret", mOptions.settings.client_secret);
        }
        const HttpResult result = http_call("POST", current->token_endpoint, headers,
                                            util::form_encode(form), mOptions.http_timeout);
        if (not result.error.empty() or result.status != 200) {
          error = result.error.empty() ? "the refresh was refused: " + oauth_error(result) : result.error;
          return;
        }
        const std::optional<JsonValue> document = JsonValue::parse(result.body);
        std::optional<TokenSet> tokens =
            document ? parse_token_response(*document, unix_now(), error) : std::nullopt;
        if (not tokens) {
          if (error.empty()) error = "the refresh answer was not JSON";
          return;
        }
        StoredToken updated = *current;
        updated.access_token = tokens->access_token;
        // Rotated refresh tokens replace the old one, which is now spent.
        if (not tokens->refresh_token.empty()) updated.refresh_token = tokens->refresh_token;
        if (not tokens->scope.empty()) updated.scope = tokens->scope;
        updated.expires_at = tokens->expires_at;
        // Saved before it is used: losing a rotated refresh token to a crash
        // would mean logging in again.
        if (not mStore.put_token_unlocked(updated, error)) return;
        mToken = updated;
        renewed = true;
      },
      error);
  if (locked and renewed) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(mOptions.credentials_path, ec);
    mStoreStamp = ec ? 0 : static_cast<int64_t>(written.time_since_epoch().count());
  }
  return renewed;
}

void OAuthSession::challenge(const std::string& www_authenticate) {
  if (www_authenticate.empty()) return;
  std::lock_guard<std::mutex> lock(mMutex);
  mChallenge = www_authenticate;
  if (insufficient_scope(www_authenticate)) {
    // Step-up: what was granted, what was asked for before, and what this
    // operation needs, together.
    std::vector<std::string> scopes;
    if (mToken) add_scopes(scopes, mToken->scope);
    add_scopes(scopes, mStepUpScope);
    add_scopes(scopes, bearer_param(www_authenticate, "scope"));
    mStepUpScope = join_scopes(scopes);
  }
}

bool OAuthSession::has_token() {
  if (mOptions.credentials_path.empty()) return false;
  std::lock_guard<std::mutex> lock(mMutex);
  reload_locked();
  return mToken.has_value();
}

bool OAuthSession::logout(std::string& error) {
  if (mOptions.credentials_path.empty()) {
    error = "there is no credential file";
    return false;
  }
  std::string why;
  const bool forgot = mStore.forget_token(mKey, why);
  std::lock_guard<std::mutex> lock(mMutex);
  mToken.reset();
  mLoaded = false;
  mStepUpScope.clear();
  if (not why.empty()) {
    error = why;
    return false;
  }
  if (not forgot) error = "not logged in";
  return forgot;
}

std::shared_ptr<PendingLogin> OAuthSession::begin_login(std::string& error) {
  if (mOptions.credentials_path.empty()) {
    error = "there is no home directory to keep the login in";
    return nullptr;
  }
  std::string challenge;
  std::string previous;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    challenge = mChallenge;
    previous = mStepUpScope;
  }

  // 1. The server's protected resource metadata, for its authorization server.
  std::optional<ResourceMetadata> resource;
  std::string why;
  for (const std::string& url :
       resource_metadata_urls(mOptions.server_url, bearer_param(challenge, "resource_metadata"))) {
    std::optional<JsonValue> document = fetch_json(url, mOptions.http_timeout, why);
    if (not document) continue;
    resource = parse_resource_metadata(*document, mOptions.server_url, why);
    if (resource) break;
  }
  std::string issuer;
  std::string resource_uri;
  if (resource) {
    issuer = resource->authorization_servers.front();
    resource_uri = resource->resource;
  } else {
    // A server from before protected resource metadata (2025-03-26) is its
    // own authorization server.
    const std::optional<util::Url> url = util::parse_url(mOptions.server_url);
    if (not url) {
      error = "the server URL is not usable";
      return nullptr;
    }
    issuer = util::url_origin(*url);
    resource_uri = canonical_resource(mOptions.server_url);
  }

  // 2. The authorization server's own metadata, naming itself exactly.
  std::optional<AuthServerMetadata> server;
  std::string server_why;
  for (const std::string& url : authorization_server_metadata_urls(issuer)) {
    std::optional<JsonValue> document = fetch_json(url, mOptions.http_timeout, server_why);
    if (not document) continue;
    server = parse_auth_server_metadata(*document, issuer, server_why);
    if (server) break;
    if (server_why.find("PKCE") != std::string::npos) break;  // found, and refused
  }
  if (not server) {
    error = "could not use the authorization server " + issuer +
            (server_why.empty() ? std::string() : ": " + server_why);
    return nullptr;
  }

  // 3. The loopback listener the browser comes back to — on the port of an
  // earlier registration when there is one, since that is the redirect URI
  // the authorization server knows.
  const bool configured = not mOptions.settings.client_id.empty();
  const bool cimd = not configured and server->client_id_metadata_document_supported and
                    not mOptions.client_metadata_url.empty();
  std::optional<StoredClient> stored =
      configured or cimd ? std::nullopt : mStore.client(server->issuer);
  int port = mOptions.settings.callback_port;
  if (port == 0 and stored and not stored->redirect_uri.empty()) {
    if (const auto url = util::parse_url(stored->redirect_uri); url and not url->port.empty()) {
      port = std::atoi(url->port.c_str());
    }
  }
  auto login = std::shared_ptr<PendingLogin>(new PendingLogin());
  login->mSession = this;
  login->mListener = std::make_unique<LoopbackListener>();
  if (not login->mListener->start(port, why)) {
    if (port == 0 or mOptions.settings.callback_port != 0) {
      error = why;
      return nullptr;
    }
    // The registered port is taken: register again on a free one.
    stored.reset();
    login->mListener = std::make_unique<LoopbackListener>();
    if (not login->mListener->start(0, why)) {
      error = why;
      return nullptr;
    }
  }
  const std::string redirect_uri =
      "http://127.0.0.1:" + std::to_string(login->mListener->port()) + "/callback";

  // 4. Who m8 is to this authorization server: configured, a metadata
  // document, an earlier registration, or a new one — in that order.
  std::string client_id;
  std::string client_secret;
  if (configured) {
    client_id = mOptions.settings.client_id;
    client_secret = mOptions.settings.client_secret;
  } else if (cimd) {
    client_id = mOptions.client_metadata_url;
  } else if (stored and stored->redirect_uri == redirect_uri and
             (stored->secret_expires_at == 0 or stored->secret_expires_at > unix_now())) {
    client_id = stored->client_id;
    client_secret = stored->client_secret;
  } else if (not server->registration_endpoint.empty()) {
    JsonValue body = JsonValue::object();
    body.set("client_name", "m8");
    JsonValue redirects = JsonValue::array();
    redirects.push_back(redirect_uri);
    body.set("redirect_uris", redirects);
    JsonValue grants = JsonValue::array();
    grants.push_back("authorization_code");
    grants.push_back("refresh_token");
    body.set("grant_types", grants);
    JsonValue responses = JsonValue::array();
    responses.push_back("code");
    body.set("response_types", responses);
    body.set("token_endpoint_auth_method", "none");
    body.set("application_type", "native");
    const HttpResult result =
        http_call("POST", server->registration_endpoint,
                  {"Content-Type: application/json", "Accept: application/json"}, body.dump(),
                  mOptions.http_timeout);
    const std::optional<JsonValue> answer = JsonValue::parse(result.body);
    if (not result.error.empty() or (result.status != 200 and result.status != 201) or
        not answer or answer->get("client_id").as_string().empty()) {
      error = "could not register m8 with " + server->issuer + ": " +
              (result.error.empty() ? oauth_error(result) : result.error);
      return nullptr;
    }
    client_id = answer->get("client_id").as_string();
    client_secret = answer->get("client_secret").as_string();
    StoredClient registered;
    registered.issuer = server->issuer;
    registered.client_id = client_id;
    registered.client_secret = client_secret;
    registered.secret_expires_at = answer->get("client_secret_expires_at").as_int();
    registered.redirect_uri = redirect_uri;
    std::string ignored;
    mStore.put_client(registered, ignored);
  } else {
    error = server->issuer + " offers m8 no way to register; register a client there yourself "
            "and set \"oauth\": {\"clientId\": ...} in the server's config";
    return nullptr;
  }

  // 5. The authorization URL: PKCE, state, the resource, the scopes.
  login->mVerifier = util::secure_random_token(32);
  login->mState = util::secure_random_token(16);
  const std::string code_challenge =
      util::base64_encode(util::sha256_bytes(login->mVerifier), /*url_safe=*/true, /*pad=*/false);
  login->mScope = not mOptions.settings.scopes.empty()
                      ? join_scopes(mOptions.settings.scopes)
                      : choose_scope(bearer_param(challenge, "scope"), previous,
                                     resource ? resource->scopes_supported : std::vector<std::string>(),
                                     server->scopes_supported);
  std::vector<std::pair<std::string, std::string>> params = {
      {"response_type", "code"},
      {"client_id", client_id},
      {"redirect_uri", redirect_uri},
      {"code_challenge", code_challenge},
      {"code_challenge_method", "S256"},
      {"state", login->mState},
      {"resource", resource_uri}};
  if (not login->mScope.empty()) params.emplace_back("scope", login->mScope);
  login->mUrl = server->authorization_endpoint +
                (server->authorization_endpoint.find('?') == std::string::npos ? "?" : "&") +
                util::form_encode(params);
  login->mIssuer = server->issuer;
  login->mIssSupported = server->iss_parameter_supported;
  login->mRedirectUri = redirect_uri;
  login->mClientId = client_id;
  login->mClientSecret = client_secret;
  const auto& methods = server->token_endpoint_auth_methods;
  login->mBasicAuth = not client_secret.empty() and
                      (methods.empty() or std::find(methods.begin(), methods.end(),
                                                    "client_secret_basic") != methods.end());
  login->mTokenEndpoint = server->token_endpoint;
  login->mResource = resource_uri;
  return login;
}

bool OAuthSession::finish(PendingLogin& login, const std::string& code, std::string& error) {
  std::vector<std::pair<std::string, std::string>> form = {
      {"grant_type", "authorization_code"},
      {"code", code},
      {"redirect_uri", login.mRedirectUri},
      {"code_verifier", login.mVerifier},
      {"resource", login.mResource}};
  std::vector<std::string> headers = {"Content-Type: application/x-www-form-urlencoded",
                                      "Accept: application/json"};
  if (login.mBasicAuth) {
    headers.push_back("Authorization: Basic " +
                      util::base64_encode(util::percent_encode(login.mClientId) + ":" +
                                          util::percent_encode(login.mClientSecret)));
  } else {
    form.emplace_back("client_id", login.mClientId);
    if (not login.mClientSecret.empty()) form.emplace_back("client_secret", login.mClientSecret);
  }
  const HttpResult result = http_call("POST", login.mTokenEndpoint, headers,
                                      util::form_encode(form), mOptions.http_timeout);
  if (not result.error.empty() or result.status != 200) {
    error = result.error.empty() ? "the authorization server refused the code: " + oauth_error(result)
                                 : result.error;
    return false;
  }
  const std::optional<JsonValue> document = JsonValue::parse(result.body);
  std::optional<TokenSet> tokens =
      document ? parse_token_response(*document, unix_now(), error) : std::nullopt;
  if (not tokens) {
    if (error.empty()) error = "the token answer was not JSON";
    return false;
  }
  StoredToken token;
  token.server_url = mKey;
  token.issuer = login.mIssuer;
  token.resource = login.mResource;
  token.client_id = login.mClientId;
  token.access_token = tokens->access_token;
  token.refresh_token = tokens->refresh_token;
  token.scope = tokens->scope.empty() ? login.mScope : tokens->scope;
  token.expires_at = tokens->expires_at;
  token.token_endpoint = login.mTokenEndpoint;
  if (not mStore.put_token(token, error)) return false;
  std::lock_guard<std::mutex> lock(mMutex);
  mToken = token;
  mLoaded = false;  // re-stamp on the next read
  mChallenge.clear();
  mStepUpScope.clear();
  return true;
}

}  // namespace mcp
