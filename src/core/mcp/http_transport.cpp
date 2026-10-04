#include <core/mcp/http_transport.h>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>

#include <core/mcp/config.h>
#include <core/mcp/oauth.h>
#include <core/mcp/schema.h>
#include <core/mcp/sse.h>
#include <core/util/text.h>
#include <core/util/url.h>

namespace mcp {

namespace {

using Headers = std::vector<std::pair<std::string, std::string>>;

// A ceiling on how long progress notifications can keep one request alive.
constexpr auto kMaxRequestLifetime = std::chrono::minutes(30);
// How often a transfer looks up from the network for a cancel, a deadline or
// close(). close() also wakes it at once.
constexpr int kSliceMs = 100;
// How much of a non-2xx body is kept: enough for any JSON-RPC error.
constexpr size_t kMaxErrorBody = 1u << 20;
// Reconnects of one legacy stream, and the wait between them when the server
// names none (or an absurd one).
constexpr int kMaxResumes = 100;
constexpr long kDefaultRetryMs = 1000;
constexpr long kMaxRetryMs = 30000;
// Finished transfers keep their connection (and TLS session) in an idle lane
// for the next request; this many are kept.
constexpr size_t kIdleLanes = 4;

void ensure_curl_initialized() {
  static const CURLcode init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  (void)init_result;
}

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string trim(std::string_view text) {
  while (not text.empty() and (text.front() == ' ' or text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (not text.empty() and (text.back() == ' ' or text.back() == '\t' or
                               text.back() == '\r' or text.back() == '\n')) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

bool has_header(const Headers& headers, std::string_view name) {
  const std::string wanted = lower(name);
  return std::any_of(headers.begin(), headers.end(),
                     [&](const auto& header) { return lower(header.first) == wanted; });
}

// A CR or LF in a header line would end it and start another.
bool header_safe(std::string_view text) {
  return text.find_first_of(std::string_view("\r\n\0", 3)) == std::string_view::npos;
}

// The headers m8 sets itself, which a configured header must not duplicate.
bool reserved_header(std::string_view name) {
  const std::string lowered = lower(name);
  return lowered == "content-type" or lowered == "accept" or lowered == "expect" or
         lowered == "mcp-protocol-version" or lowered == "mcp-method" or
         lowered == "mcp-name" or lowered == "mcp-session-id" or
         lowered == "last-event-id" or lowered.rfind("mcp-param-", 0) == 0;
}

// What the spec allows in a session id: visible ASCII.
bool visible_ascii(std::string_view text) {
  return not text.empty() and std::all_of(text.begin(), text.end(), [](char c) {
           return c >= 0x21 and c <= 0x7E;
         });
}

std::string clipped(std::string_view text, size_t limit) {
  std::string out = util::sanitize_utf8(text.substr(0, std::min(text.size(), limit)));
  std::replace(out.begin(), out.end(), '\n', ' ');
  if (text.size() > limit) out += "...";
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (const std::string& part : parts) {
    if (not out.empty()) out += separator;
    out += part;
  }
  return out;
}

}  // namespace

// ─────────────────────────────── plumbing ───────────────────────────────────

// A multi handle and the easy handle that runs in it. Keeping the pair
// between requests keeps the multi handle's connection cache, so the next
// request reuses the connection (and its TLS session) instead of dialling.
struct HttpTransport::Lane {
  CURLM* multi = curl_multi_init();
  CURL* easy = curl_easy_init();
  char error[CURL_ERROR_SIZE] = {};

  ~Lane() {
    if (easy != nullptr) curl_easy_cleanup(easy);
    if (multi != nullptr) curl_multi_cleanup(multi);
  }
};

// One HTTP request and what came back for it, filled in by curl's callbacks
// on the thread running the transfer.
struct HttpTransport::Exchange {
  explicit Exchange(HttpTransport& transport)
      : self(transport), parser(transport.mConfig.max_event_bytes) {}

  HttpTransport& self;
  std::string id_key;  // the request whose response ends this exchange
  const std::atomic<bool>* cancel = nullptr;
  Clock::time_point started = Clock::now();
  Clock::time_point deadline = Clock::time_point::max();
  Clock::duration timeout{};  // what a progress notification extends by
  // A request's deadline stands still while a person answers the server;
  // what the transport sends on its own (notifications, a DELETE) does not.
  bool pausable = false;
  Clock::duration paused_at_start{};

  // The response head.
  long status = 0;
  std::string content_type;
  std::string session_id;
  std::string location;
  std::vector<std::string> www_authenticate;

  // The body: an SSE stream, or JSON (or whatever an error page holds).
  bool decided = false;
  bool sse = false;
  SseParser parser;
  std::string body;
  size_t max_body = 0;
  bool overflow = false;
  std::string last_event_id;
  long retry_ms = -1;

  std::optional<RpcMessage> answer;

  void reset_response() {
    status = 0;
    content_type.clear();
    session_id.clear();
    location.clear();
    www_authenticate.clear();
    decided = false;
    sse = false;
    parser = SseParser(self.mConfig.max_event_bytes);
    body.clear();
    overflow = false;
  }
};

HttpTransport::HttpTransport(HttpConfig config, TransportHandlers handlers)
    : mConfig(std::move(config)), mHandlers(std::move(handlers)) {
  ensure_curl_initialized();
}

HttpTransport::~HttpTransport() {
  begin_close();
  wait_closed();
}

bool HttpTransport::start(std::string& error) {
  std::string why;
  const std::optional<util::Url> url = util::parse_url(mConfig.url, &why);
  if (not url) {
    error = "the MCP server URL '" + mConfig.url + "' is not usable: " + why;
    return false;
  }
  if (url->scheme != "http" and url->scheme != "https") {
    error = "an MCP server URL must be http or https, not " + url->scheme;
    return false;
  }
  if (url->has_userinfo) {
    // curl would send it as Basic auth, and the URL is shown in /mcp.
    error = "put credentials for the MCP server in a header, not in its URL";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mStarted = true;
  }
  mWorker = std::thread([this] { worker_loop(); });
  return true;
}

std::unique_ptr<HttpTransport::Lane> HttpTransport::acquire_lane() {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (not mIdleLanes.empty()) {
      std::unique_ptr<Lane> lane = std::move(mIdleLanes.back());
      mIdleLanes.pop_back();
      return lane;
    }
  }
  return std::make_unique<Lane>();
}

void HttpTransport::release_lane(std::unique_ptr<Lane> lane) {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (not mClosing and mIdleLanes.size() < kIdleLanes) {
      mIdleLanes.push_back(std::move(lane));
      return;
    }
  }
  // Destroyed here, outside the lock: closing its connections may take a moment.
}

bool HttpTransport::late(const Exchange& exchange) const {
  if (exchange.deadline == Clock::time_point::max()) return false;
  if (not exchange.pausable) return Clock::now() >= exchange.deadline;
  std::lock_guard<std::mutex> lock(mMutex);
  if (mInteractions > 0) return false;
  return Clock::now() >= exchange.deadline + (mPaused - exchange.paused_at_start);
}

HttpTransport::Stop HttpTransport::transfer(Exchange& exchange, Verb verb,
                                            const std::string& body,
                                            const Headers& extra, bool closable,
                                            std::string& error) {
  std::unique_ptr<Lane> lane = acquire_lane();
  if (lane->multi == nullptr or lane->easy == nullptr) {
    error = "could not set up an HTTP request";
    return Stop::None;
  }
  CURL* easy = lane->easy;
  curl_easy_reset(easy);
  lane->error[0] = '\0';

  // ── headers ──
  Headers headers;
  if (verb == Verb::Post) {
    headers.emplace_back("Content-Type", "application/json");
    headers.emplace_back("Accept", "application/json, text/event-stream");
  } else if (verb == Verb::Get) {
    headers.emplace_back("Accept", "text/event-stream");
  }
  for (const auto& header : extra) headers.push_back(header);
  for (const auto& [name, value] : mConfig.headers) {
    if (reserved_header(name) or not header_safe(name) or not header_safe(value)) continue;
    headers.emplace_back(name, value);
  }
  if (not has_header(mConfig.headers, "Authorization") and mConfig.authorization) {
    const std::string authorization = mConfig.authorization();
    if (not authorization.empty() and header_safe(authorization)) {
      headers.emplace_back("Authorization", authorization);
    }
  }
  curl_slist* list = nullptr;
  // "Expect:" with no value stops curl's Expect: 100-continue round trip on
  // large bodies.
  list = curl_slist_append(list, "Expect:");
  for (const auto& [name, value] : headers) {
    list = curl_slist_append(list, (name + ": " + value).c_str());
  }

  // ── callbacks ──
  const auto on_header = +[](char* buffer, size_t size, size_t count, void* data) -> size_t {
    auto& ex = *static_cast<Exchange*>(data);
    const size_t bytes = size * count;
    const std::string line = trim(std::string_view(buffer, bytes));
    if (line.rfind("HTTP/", 0) == 0) {
      // A response head begins; a 1xx one may come before the real one.
      const size_t space = line.find(' ');
      ex.status = space == std::string::npos ? 0 : std::strtol(line.c_str() + space + 1, nullptr, 10);
      ex.content_type.clear();
      ex.session_id.clear();
      ex.location.clear();
      ex.www_authenticate.clear();
      return bytes;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return bytes;
    const std::string name = lower(trim(std::string_view(line).substr(0, colon)));
    const std::string value = trim(std::string_view(line).substr(colon + 1));
    if (name == "content-type") {
      ex.content_type = lower(value);
    } else if (name == "mcp-session-id") {
      ex.session_id = value;
    } else if (name == "location") {
      ex.location = value;
    } else if (name == "www-authenticate") {
      ex.www_authenticate.push_back(value);
    }
    return bytes;
  };
  const auto on_body = +[](char* data, size_t size, size_t count, void* user) -> size_t {
    auto& ex = *static_cast<Exchange*>(user);
    const size_t bytes = size * count;
    if (ex.answer) return bytes;  // what follows our answer is not ours
    if (not ex.decided) {
      ex.decided = true;
      const bool success = ex.status / 100 == 2;
      ex.sse = success and ex.content_type.rfind("text/event-stream", 0) == 0;
      ex.max_body = success ? ex.self.mConfig.max_body_bytes : kMaxErrorBody;
    }
    if (ex.sse) {
      ex.parser.feed(std::string_view(data, bytes), [&](const SseEvent& event) {
        ex.self.on_event(ex, event.data);
      });
      if (not ex.parser.last_event_id().empty()) ex.last_event_id = ex.parser.last_event_id();
      if (ex.parser.retry_ms() >= 0) ex.retry_ms = ex.parser.retry_ms();
      return bytes;
    }
    if (ex.body.size() + bytes > ex.max_body) {
      ex.overflow = true;
      return 0;  // ends the transfer with CURLE_WRITE_ERROR
    }
    ex.body.append(data, bytes);
    return bytes;
  };

  // ── options ──
  curl_easy_setopt(easy, CURLOPT_URL, mConfig.url.c_str());
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(mConfig.connect_timeout.count()));
  curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(easy, CURLOPT_USERAGENT,
                   (std::string(kClientName) + "/" + kClientVersion).c_str());
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, lane->error);
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(easy, CURLOPT_HEADERDATA, &exchange);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &exchange);
  switch (verb) {
    case Verb::Post:
      curl_easy_setopt(easy, CURLOPT_POST, 1L);
      curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
      curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
      break;
    case Verb::Get:
      curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
      break;
    case Verb::Delete:
      curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
      break;
  }

  // ── run, a slice at a time ──
  Stop stop = Stop::None;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (closable and mClosing) stop = Stop::Closing;
    else mActive.push_back(lane.get());
  }
  bool finished = false;
  CURLcode result = CURLE_OK;
  if (stop == Stop::None) {
    curl_multi_add_handle(lane->multi, easy);
    while (true) {
      int running = 0;
      const CURLMcode code = curl_multi_perform(lane->multi, &running);
      if (code != CURLM_OK) {
        error = curl_multi_strerror(code);
        break;
      }
      int left = 0;
      while (CURLMsg* message = curl_multi_info_read(lane->multi, &left)) {
        if (message->msg == CURLMSG_DONE) {
          result = message->data.result;
          finished = true;
        }
      }
      // An answer wins over a cancel or a deadline that arrived with it.
      if (exchange.answer) {
        stop = Stop::Answered;
        break;
      }
      if (finished or running == 0) break;
      if (exchange.cancel != nullptr and exchange.cancel->load()) {
        stop = Stop::Cancelled;
        break;
      }
      {
        std::lock_guard<std::mutex> lock(mMutex);
        if (closable and mClosing) {
          stop = Stop::Closing;
          break;
        }
      }
      if (late(exchange)) {
        stop = Stop::Deadline;
        break;
      }
      curl_multi_poll(lane->multi, nullptr, 0, kSliceMs, nullptr);
    }
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mActive.erase(std::remove(mActive.begin(), mActive.end(), lane.get()), mActive.end());
    }
    // Mid-transfer this closes the connection: for a 2026-07-28 stream, that
    // is the cancellation.
    curl_multi_remove_handle(lane->multi, easy);
  } else {
    std::lock_guard<std::mutex> lock(mMutex);
    mActive.erase(std::remove(mActive.begin(), mActive.end(), lane.get()), mActive.end());
  }
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, nullptr);
  curl_slist_free_all(list);

  if (exchange.sse and stop == Stop::None) {
    // End of stream: an event without its closing blank line still counts.
    exchange.parser.finish([&](const SseEvent& event) { on_event(exchange, event.data); });
    if (not exchange.parser.last_event_id().empty()) {
      exchange.last_event_id = exchange.parser.last_event_id();
    }
    if (exchange.answer) stop = Stop::Answered;
  }
  if (finished and result != CURLE_OK and not exchange.overflow and not exchange.answer) {
    error = lane->error[0] != '\0' ? std::string(lane->error) : curl_easy_strerror(result);
  }
  release_lane(std::move(lane));
  return stop;
}

// ─────────────────────────────── requests ───────────────────────────────────

ReplyFuture HttpTransport::send(RequestSpec spec) {
  std::promise<Reply> promise;
  promise.set_value(exchange(spec));
  return promise.get_future().share();
}

std::vector<std::pair<std::string, std::string>> HttpTransport::protocol_headers(
    const std::string& method, bool initialize) const {
  Headers out;
  // initialize negotiates its version in the body and starts a session, so
  // it carries neither.
  if (initialize) return out;
  std::lock_guard<std::mutex> lock(mMutex);
  if (not mVersion.empty()) out.emplace_back("MCP-Protocol-Version", mVersion);
  if (mEra == Era::Modern) {
    if (not method.empty()) out.emplace_back("Mcp-Method", method);
  } else if (not mSessionId.empty()) {
    out.emplace_back("Mcp-Session-Id", mSessionId);
  }
  return out;
}

Reply HttpTransport::exchange(const RequestSpec& spec) {
  Reply reply;
  Era era = Era::Unknown;
  bool sent_session = false;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mClosing or mDead or not mStarted) {
      reply.error = mExitReason.empty() ? "the connection to the MCP server is closed"
                                        : mExitReason;
      reply.exited = true;
      return reply;
    }
    ++mInFlight;
    era = mEra;
    sent_session = mEra != Era::Modern and not mSessionId.empty();
  }
  struct Done {
    HttpTransport& transport;
    ~Done() {
      {
        std::lock_guard<std::mutex> lock(transport.mMutex);
        --transport.mInFlight;
      }
      transport.mIdle.notify_all();
    }
  } done{*this};

  const bool initialize = spec.method == "initialize";
  if (initialize) sent_session = false;

  Exchange ex(*this);
  ex.id_key = id_key(spec.id);
  ex.cancel = spec.cancel;
  ex.pausable = true;
  ex.deadline = spec.deadline;
  if (spec.deadline != Clock::time_point::max()) ex.timeout = spec.deadline - ex.started;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    ex.paused_at_start = mPaused;
  }

  Headers headers = protocol_headers(spec.method, initialize);
  if (era == Era::Modern and not initialize) {
    if (not spec.name.empty()) headers.emplace_back("Mcp-Name", encode_header_value(spec.name));
    for (const auto& [name, value] : spec.headers) {
      if (header_safe(name) and header_safe(value)) headers.emplace_back(name, value);
    }
  }
  const std::string body = make_request(spec.id, spec.method, spec.params);

  Verb verb = Verb::Post;
  Headers resume_headers;
  bool renewed = false;
  for (int resumes = 0;; ++resumes) {
    std::string error;
    Stop stop = transfer(ex, verb, verb == Verb::Post ? body : std::string(),
                         verb == Verb::Post ? headers : resume_headers, true, error);

    if (stop == Stop::Cancelled or stop == Stop::Deadline or stop == Stop::Closing) {
      reply.timed_out = stop == Stop::Deadline;
      reply.cancelled = not reply.timed_out;
      reply.error = stop == Stop::Deadline  ? "the server did not answer in time"
                    : stop == Stop::Closing ? "the connection to the MCP server was closed"
                                            : "cancelled";
      // A 2026-07-28 server took the closed stream as the cancellation.
      // Earlier ones do not read a dropped connection that way, and are told.
      if (era == Era::Legacy and stop != Stop::Closing) {
        cancel_on_wire(spec.id, reply.timed_out ? "timed out" : "cancelled by the client");
      }
      return reply;
    }

    reply.http_status = ex.status;
    // Before the body: both often come with a JSON-RPC error that would
    // otherwise pass for the server's answer.
    if (error.empty() and ex.status == 401) {
      reply.www_authenticate = join(ex.www_authenticate, ", ");
      // An expired or revoked token: renewed, the request goes once more.
      if (mConfig.renew and not renewed and verb == Verb::Post) {
        renewed = true;
        if (mConfig.renew(reply.www_authenticate)) {
          ex.reset_response();
          continue;
        }
      }
      reply.error = "the server requires authorization (HTTP 401)";
      return reply;
    }
    if (error.empty() and ex.status == 403 and
        insufficient_scope(join(ex.www_authenticate, ", "))) {
      reply.www_authenticate = join(ex.www_authenticate, ", ");
      reply.error = "the server wants more permissions than this login has (HTTP 403)";
      return reply;
    }
    if (error.empty() and ex.status == 404 and sent_session) {
      // The session is gone (the server restarted or expired it) and the
      // request was never run; the client starts a new session and resends.
      {
        std::lock_guard<std::mutex> lock(mMutex);
        mDead = true;
        mExitReason = "the server ended the MCP session";
      }
      reply.session_expired = true;
      reply.error = "the server ended the MCP session";
      return reply;
    }
    std::optional<RpcMessage> message = std::move(ex.answer);
    if (not message and not ex.sse and error.empty() and not ex.overflow and
        not ex.body.empty()) {
      RpcMessage parsed = parse_message(ex.body);
      // Ours, or an error about the request (servers answer a request they
      // could not read with a null id).
      if (parsed.kind == MessageKind::Error or
          (parsed.kind == MessageKind::Result and id_key(parsed.id) == ex.id_key)) {
        message = std::move(parsed);
      }
    }
    if (message) {
      if (initialize and ex.status / 100 == 2 and message->kind == MessageKind::Result and
          not ex.session_id.empty()) {
        if (not visible_ascii(ex.session_id)) {
          reply.error = "the server sent an Mcp-Session-Id m8 cannot send back";
          return reply;
        }
        std::lock_guard<std::mutex> lock(mMutex);
        mSessionId = ex.session_id;
      }
      reply.ok = true;
      reply.message = std::move(*message);
      return reply;
    }

    if (not error.empty()) {
      reply.error = "could not reach the MCP server at " + mConfig.url + ": " + error;
      return reply;
    }
    if (ex.overflow) {
      reply.error = "the server's answer was larger than " +
                    std::to_string(mConfig.max_body_bytes >> 20) + " MiB";
      return reply;
    }
    if (ex.status / 100 == 3) {
      reply.error = "the MCP server answered with a redirect (HTTP " +
                    std::to_string(ex.status) + ")" +
                    (ex.location.empty() ? std::string() : " to " + clipped(ex.location, 300)) +
                    "; m8 does not follow redirects — put the new URL in the config";
      return reply;
    }
    if (ex.sse and ex.status / 100 == 2) {
      // The stream ended before our answer. A 2025-11-25 server may close it
      // on purpose and expect the client to come back for the rest.
      if (era == Era::Legacy and not ex.last_event_id.empty() and resumes < kMaxResumes) {
        long wait = ex.retry_ms >= 0 ? ex.retry_ms : kDefaultRetryMs;
        wait = std::min(wait, kMaxRetryMs);
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(wait);
        Stop paused = Stop::None;
        while (Clock::now() < until) {
          if (spec.cancel != nullptr and spec.cancel->load()) paused = Stop::Cancelled;
          else if (late(ex)) paused = Stop::Deadline;
          std::unique_lock<std::mutex> lock(mMutex);
          if (mClosing) paused = Stop::Closing;
          if (paused != Stop::None) break;
          mIdle.wait_for(lock, std::chrono::milliseconds(std::min<long>(wait, kSliceMs)));
        }
        if (paused != Stop::None) {
          reply.timed_out = paused == Stop::Deadline;
          reply.cancelled = not reply.timed_out;
          reply.error = reply.timed_out ? "the server did not answer in time" : "cancelled";
          if (paused != Stop::Closing) {
            cancel_on_wire(spec.id, reply.timed_out ? "timed out" : "cancelled by the client");
          }
          return reply;
        }
        resume_headers = protocol_headers(std::string(), false);
        resume_headers.emplace_back("Last-Event-ID", ex.last_event_id);
        verb = Verb::Get;
        ex.reset_response();
        continue;
      }
      reply.error = "the server ended its event stream without answering";
      return reply;
    }
    if (ex.status / 100 == 2) {
      reply.error = ex.body.empty() ? "the server sent no answer (HTTP " +
                                          std::to_string(ex.status) + ")"
                                    : "the server's answer is not a JSON-RPC response: " +
                                          clipped(ex.body, 200);
      return reply;
    }
    reply.error = "HTTP " + std::to_string(ex.status) +
                  (ex.body.empty() ? std::string() : ": " + clipped(ex.body, 300));
    return reply;
  }
}

void HttpTransport::on_event(Exchange& exchange, const std::string& data) {
  if (data.empty()) return;  // a priming event: only its id mattered
  RpcMessage message = parse_message(data);
  switch (message.kind) {
    case MessageKind::Result:
    case MessageKind::Error:
      // On a stream scoped to our request, an error with a null id is about it.
      if (not exchange.answer and
          (id_key(message.id) == exchange.id_key or
           (message.kind == MessageKind::Error and message.id.is_null()))) {
        exchange.answer = std::move(message);
      }
      return;
    case MessageKind::Notification:
      if (message.method == "notifications/progress") {
        // The request's own id is its progress token.
        if (id_key(message.params.get("progressToken")) == exchange.id_key and
            exchange.timeout > Clock::duration::zero()) {
          exchange.deadline = std::min(Clock::now() + exchange.timeout,
                                       exchange.started + kMaxRequestLifetime);
        }
        return;
      }
      if (mHandlers.on_notification) mHandlers.on_notification(message);
      return;
    case MessageKind::Request: {
      // Only servers before 2026-07-28 send requests; 2026-07-28 asks for
      // input inside its results instead.
      std::lock_guard<std::mutex> lock(mMutex);
      if (mClosing or mEra == Era::Modern) return;
      mInbox.push_back(std::move(message));
      mInboxCv.notify_one();
      return;
    }
    case MessageKind::Invalid:
    default:
      return;
  }
}

bool HttpTransport::post_message(const std::string& body, const std::string& method,
                                 std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mClosing or mDead or not mStarted) {
      error = "the connection to the MCP server is closed";
      return false;
    }
    ++mInFlight;
  }
  struct Done {
    HttpTransport& transport;
    ~Done() {
      {
        std::lock_guard<std::mutex> lock(transport.mMutex);
        --transport.mInFlight;
      }
      transport.mIdle.notify_all();
    }
  } done{*this};

  Exchange ex(*this);
  ex.deadline = Clock::now() + mConfig.message_timeout;
  const Stop stop = transfer(ex, Verb::Post, body, protocol_headers(method, false), true, error);
  if (stop != Stop::None and stop != Stop::Answered) {
    error = stop == Stop::Deadline ? "the server did not accept it in time" : "cancelled";
    return false;
  }
  if (not error.empty()) return false;
  if (ex.status / 100 != 2) {
    error = "HTTP " + std::to_string(ex.status);
    return false;
  }
  return true;
}

bool HttpTransport::notify(const std::string& method, const util::JsonValue& params) {
  std::string error;
  return post_message(make_notification(method, params), method, error);
}

void HttpTransport::cancel_on_wire(const util::JsonValue& id, const std::string& reason) {
  util::JsonValue params = util::JsonValue::object();
  params.set("requestId", id);
  params.set("reason", reason);
  notify("notifications/cancelled", params);
}

// ─────────────────────── what legacy servers ask of us ──────────────────────

void HttpTransport::worker_loop() {
  while (true) {
    RpcMessage request;
    bool ping = false;
    {
      std::unique_lock<std::mutex> lock(mMutex);
      mInboxCv.wait(lock, [&] { return not mInbox.empty() or mClosing; });
      if (mClosing) return;
      request = std::move(mInbox.front());
      mInbox.pop_front();
      ping = request.method == "ping";
      if (not ping and mInteractions++ == 0) mInteractionStarted = Clock::now();
    }

    util::JsonValue result = util::JsonValue::object();
    RpcError error;
    if (not ping) {
      if (mHandlers.on_request) {
        mHandlers.on_request(request, result, error);
      } else {
        error.code = kMethodNotFound;
        error.message = "m8 does not support " + request.method;
      }
      std::lock_guard<std::mutex> lock(mMutex);
      if (--mInteractions == 0) mPaused += Clock::now() - mInteractionStarted;
    }

    std::string why;
    post_message(error.code != 0 ? make_error(request.id, error.code, error.message)
                                 : make_result(request.id, result),
                 std::string(), why);
  }
}

// ──────────────────────────────── closing ───────────────────────────────────

void HttpTransport::begin_close() {
  bool end_session = false;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mClosing) return;
    mClosing = true;
    if (mExitReason.empty()) mExitReason = "the connection to the MCP server was closed";
    for (Lane* lane : mActive) curl_multi_wakeup(lane->multi);
    end_session = mStarted and not mDead and mEra != Era::Modern and not mSessionId.empty();
  }
  mInboxCv.notify_all();
  mIdle.notify_all();
  if (end_session) mCloser = std::thread([this] { delete_session(); });
}

void HttpTransport::delete_session() {
  Exchange ex(*this);
  ex.deadline = Clock::now() + mConfig.close_timeout;
  std::string error;
  // 405 means the server keeps sessions until they expire; nothing more to do.
  transfer(ex, Verb::Delete, std::string(), protocol_headers(std::string(), false),
           /*closable=*/false, error);
}

void HttpTransport::wait_closed() {
  begin_close();
  if (mCloser.joinable()) mCloser.join();
  {
    std::unique_lock<std::mutex> lock(mMutex);
    mIdle.wait(lock, [&] { return mInFlight == 0; });
  }
  if (mWorker.joinable()) mWorker.join();
  std::vector<std::unique_ptr<Lane>> lanes;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    lanes.swap(mIdleLanes);
  }
}

bool HttpTransport::alive() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mStarted and not mClosing and not mDead;
}

void HttpTransport::set_protocol(Era era, const std::string& version) {
  std::lock_guard<std::mutex> lock(mMutex);
  mEra = era;
  mVersion = version;
}

std::string HttpTransport::exit_reason() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mExitReason;
}

std::string HttpTransport::session_id() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mSessionId;
}

std::unique_ptr<Transport> make_http_transport(const ServerConfig& expanded,
                                               TransportHandlers handlers,
                                               std::shared_ptr<OAuthSession> oauth) {
  HttpConfig config;
  config.server = expanded.name;
  config.url = expanded.url;
  config.headers = expanded.headers;
  if (oauth) {
    config.authorization = [oauth] { return oauth->authorization(); };
    config.renew = [oauth](const std::string& challenge) {
      oauth->challenge(challenge);
      return oauth->renew_after_rejection();
    };
  }
  return std::make_unique<HttpTransport>(std::move(config), std::move(handlers));
}

}  // namespace mcp
