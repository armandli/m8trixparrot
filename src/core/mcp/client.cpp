#include <core/mcp/client.h>

#include <algorithm>
#include <thread>

#include <core/mcp/oauth.h>

namespace mcp {

namespace {

constexpr int kMaxPages = 50;
constexpr size_t kMaxItems = 1000;
constexpr int kMaxInputRounds = 8;

bool contains(const util::JsonValue& array, std::string_view value) {
  for (const util::JsonValue& item : array.items()) {
    if (item.as_string() == value) return true;
  }
  return false;
}

std::string first_legacy(const util::JsonValue& versions) {
  for (const util::JsonValue& item : versions.items()) {
    if (is_known_legacy_version(item.as_string())) return item.as_string();
  }
  return std::string();
}

std::string version_list(const util::JsonValue& versions) {
  std::string out;
  for (const util::JsonValue& item : versions.items()) {
    out += (out.empty() ? "" : ", ") + item.as_string();
  }
  return out;
}

}  // namespace

Client::Client(ClientOptions options) : mOptions(std::move(options)) {}

Client::~Client() { close(); }

util::JsonValue Client::next_id() { return util::JsonValue(mNextId.fetch_add(1)); }

util::JsonValue Client::client_capabilities() const {
  util::JsonValue capabilities = util::JsonValue::object();
  if (mOptions.elicit) {
    util::JsonValue elicitation = util::JsonValue::object();
    elicitation.set("form", util::JsonValue::object());
    elicitation.set("url", util::JsonValue::object());
    capabilities.set("elicitation", std::move(elicitation));
  }
  return capabilities;
}

util::JsonValue Client::request_meta(bool progress,
                                     const util::JsonValue& id) const {
  util::JsonValue meta = util::JsonValue::object();
  meta.set("io.modelcontextprotocol/protocolVersion", kModernVersion);
  util::JsonValue client = util::JsonValue::object();
  client.set("name", kClientName);
  client.set("version", kClientVersion);
  meta.set("io.modelcontextprotocol/clientInfo", std::move(client));
  meta.set("io.modelcontextprotocol/clientCapabilities", client_capabilities());
  if (progress) meta.set("progressToken", id);
  return meta;
}

// ───────────────────────────── era detection ────────────────────────────────

Client::Handshake Client::accept_discover(const RpcMessage& message) {
  Handshake outcome;
  const util::JsonValue& result = message.result;
  const util::JsonValue& versions = result.get("supportedVersions");
  if (not contains(versions, kModernVersion)) {
    outcome.error = "the server supports protocol versions " +
                    version_list(versions) + "; m8 speaks " + kModernVersion +
                    " and the initialize-based versions";
    return outcome;
  }
  std::lock_guard<std::mutex> lock(mMutex);
  mEra = Era::Modern;
  mVersion = kModernVersion;
  mCapabilities = parse_capabilities(result.get("capabilities"));
  const util::JsonValue& info =
      result.get("_meta").get("io.modelcontextprotocol/serverInfo");
  mInfo.name = info.get("name").as_string();
  mInfo.version = info.get("version").as_string();
  mInfo.title = info.get("title").as_string();
  mInstructions = result.get("instructions").as_string();
  outcome.ok = true;
  return outcome;
}

Client::Handshake Client::accept_initialize(Transport& transport,
                                            const RpcMessage& message) {
  Handshake outcome;
  const util::JsonValue& result = message.result;
  const std::string version = result.get("protocolVersion").as_string();
  if (not is_known_legacy_version(version)) {
    outcome.error = "the server answered initialize with protocol version '" +
                    version + "', which m8 does not speak";
    return outcome;
  }
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mEra = Era::Legacy;
    mVersion = version;
    mCapabilities = parse_capabilities(result.get("capabilities"));
    const util::JsonValue& info = result.get("serverInfo");
    mInfo.name = info.get("name").as_string();
    mInfo.version = info.get("version").as_string();
    mInfo.title = info.get("title").as_string();
    mInstructions = result.get("instructions").as_string();
  }
  transport.set_protocol(Era::Legacy, version);
  transport.notify("notifications/initialized", util::JsonValue());
  outcome.ok = true;
  return outcome;
}

Client::Handshake Client::legacy_initialize(Transport& transport,
                                            Clock::time_point deadline) {
  util::JsonValue params = util::JsonValue::object();
  params.set("protocolVersion", kLatestLegacyVersion);
  params.set("capabilities", client_capabilities());
  util::JsonValue client = util::JsonValue::object();
  client.set("name", kClientName);
  client.set("version", kClientVersion);
  params.set("clientInfo", std::move(client));

  RequestSpec spec;
  spec.id = next_id();
  spec.method = "initialize";
  spec.params = std::move(params);
  spec.deadline = deadline;
  const Reply reply = transport.request(std::move(spec));

  Handshake outcome;
  if (reply.http_status == 401) {
    outcome.needs_auth = true;
    outcome.www_authenticate = reply.www_authenticate;
    outcome.error = "the server requires you to log in";
    return outcome;
  }
  if (not reply.ok) {
    outcome.error = "initialize failed: " + reply.error;
    return outcome;
  }
  if (reply.message.kind != MessageKind::Result) {
    outcome.error = "initialize failed: " + describe(reply.message.error);
    return outcome;
  }
  return accept_initialize(transport, reply.message);
}

Client::Handshake Client::handshake(Transport& transport) {
  const Clock::time_point deadline = Clock::now() + mOptions.startup_timeout;

  const bool force_legacy =
      mOptions.protocol == "legacy" or
      (mOptions.protocol == "auto" and mOptions.remembered_era == Era::Legacy);
  if (force_legacy) {
    Handshake legacy = legacy_initialize(transport, deadline);
    // A remembered answer that has stopped being true is re-probed; a forced
    // one is the user's to fix.
    if (legacy.ok or mOptions.protocol == "legacy" or legacy.needs_auth) {
      return legacy;
    }
  }

  RequestSpec discover;
  discover.id = next_id();
  discover.method = "server/discover";
  discover.params = util::JsonValue::object();
  discover.params.set("_meta", request_meta());
  discover.deadline = deadline;
  transport.set_protocol(Era::Modern, kModernVersion);

  // A modern-only answer: what a server that rejected our version supports.
  const auto modern_error = [&](const RpcMessage& message) -> Handshake {
    Handshake outcome;
    if (message.error.code == kUnsupportedProtocolVersion) {
      const util::JsonValue& supported = message.error.data.get("supported");
      if (not first_legacy(supported).empty()) {
        return legacy_initialize(transport, deadline);
      }
      outcome.error = "the server supports protocol versions " +
                      version_list(supported) + "; m8 speaks " + kModernVersion;
      return outcome;
    }
    outcome.error = "server/discover failed: " + describe(message.error);
    return outcome;
  };

  if (mOptions.http) {
    const Reply reply = transport.request(std::move(discover));
    if (reply.http_status == 401) {
      Handshake outcome;
      outcome.needs_auth = true;
      outcome.www_authenticate = reply.www_authenticate;
      outcome.error = "the server requires you to log in";
      return outcome;
    }
    if (reply.ok and reply.message.kind == MessageKind::Result) {
      return accept_discover(reply.message);
    }
    if (reply.ok and reply.message.kind == MessageKind::Error and
        is_modern_error(reply.message.error.code)) {
      return modern_error(reply.message);
    }
    // A network failure or a 5xx is the server being down, not a sign of its
    // era; only a 4xx (or a JSON-RPC error that is not a modern one, such as
    // -32601 for the discover method every modern server must implement)
    // means an older server.
    const bool server_error = reply.http_status >= 500 or
                              (reply.http_status == 0 and not reply.ok);
    if (server_error or mOptions.protocol == "modern") {
      Handshake outcome;
      outcome.error = reply.ok ? describe(reply.message.error) : reply.error;
      return outcome;
    }
    return legacy_initialize(transport, deadline);
  }

  ReplyFuture discovered = transport.send(std::move(discover));
  if (discovered.wait_for(mOptions.probe_timeout) == std::future_status::ready) {
    const Reply reply = discovered.get();
    if (reply.ok and reply.message.kind == MessageKind::Result) {
      return accept_discover(reply.message);
    }
    if (reply.ok and reply.message.kind == MessageKind::Error and
        is_modern_error(reply.message.error.code)) {
      return modern_error(reply.message);
    }
    if (reply.exited) {
      // Some older SDKs crash on a request before initialize. The caller
      // starts a fresh process and goes straight to the legacy handshake.
      Handshake outcome;
      outcome.error = "exited during the protocol probe: " + reply.error;
      return outcome;
    }
    if (mOptions.protocol == "modern") {
      Handshake outcome;
      outcome.error = reply.ok ? describe(reply.message.error) : reply.error;
      return outcome;
    }
    return legacy_initialize(transport, deadline);
  }

  if (mOptions.protocol == "modern") {
    const Reply reply = discovered.get();
    if (reply.ok and reply.message.kind == MessageKind::Result) {
      return accept_discover(reply.message);
    }
    Handshake outcome;
    outcome.error = reply.ok ? describe(reply.message.error) : reply.error;
    return outcome;
  }

  // No answer yet: silent legacy server, or a modern one still starting. Ask
  // the legacy question too and take whichever answer comes first.
  RequestSpec initialize;
  initialize.id = next_id();
  initialize.method = "initialize";
  initialize.params = util::JsonValue::object();
  initialize.params.set("protocolVersion", kLatestLegacyVersion);
  initialize.params.set("capabilities", client_capabilities());
  util::JsonValue client = util::JsonValue::object();
  client.set("name", kClientName);
  client.set("version", kClientVersion);
  initialize.params.set("clientInfo", std::move(client));
  initialize.deadline = deadline;
  ReplyFuture initialized = transport.send(std::move(initialize));

  bool discover_failed = false;
  bool initialize_failed = false;
  std::string last_error;
  while (not(discover_failed and initialize_failed)) {
    if (not discover_failed and
        discovered.wait_for(std::chrono::milliseconds(10)) ==
            std::future_status::ready) {
      const Reply reply = discovered.get();
      if (reply.ok and reply.message.kind == MessageKind::Result) {
        return accept_discover(reply.message);
      }
      discover_failed = true;
      if (not reply.ok) last_error = reply.error;
    }
    if (not initialize_failed and
        initialized.wait_for(std::chrono::milliseconds(10)) ==
            std::future_status::ready) {
      const Reply reply = initialized.get();
      if (reply.ok and reply.message.kind == MessageKind::Result) {
        return accept_initialize(transport, reply.message);
      }
      initialize_failed = true;
      last_error = reply.ok ? describe(reply.message.error) : reply.error;
    }
  }
  Handshake outcome;
  outcome.error = "the server answered neither server/discover nor initialize";
  if (not last_error.empty()) outcome.error += " (" + last_error + ")";
  return outcome;
}

ConnectResult Client::connect() {
  std::lock_guard<std::mutex> connect_lock(mConnectMutex);
  ConnectResult result;

  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mClosing) {
      result.error = "the MCP client is shutting down";
      return result;
    }
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    TransportHandlers handlers;
    handlers.on_notification = [this](const RpcMessage& message) {
      on_notification(message);
    };
    handlers.on_request = [this](const RpcMessage& request,
                                 util::JsonValue& reply, RpcError& error) {
      on_server_request(request, reply, error);
    };
    std::shared_ptr<Transport> transport = mOptions.make_transport(handlers);
    if (not transport) {
      result.error = "no transport for this server";
      return result;
    }
    std::string error;
    if (not transport->start(error)) {
      result.error = error;
      return result;
    }
    {
      std::lock_guard<std::mutex> lock(mMutex);
      if (mClosing) {
        transport->begin_close();
        result.error = "the MCP client is shutting down";
        return result;
      }
      mConnecting = transport;
    }

    if (attempt == 1) mOptions.protocol = "legacy";
    const Handshake shaken = handshake(*transport);
    bool closing = false;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mConnecting.reset();
      closing = mClosing;
      if (shaken.ok and not closing) {
        mTransport = std::move(transport);
        result.ok = true;
        return result;
      }
    }
    if (closing) {
      transport->close();
      result.error = "the MCP client is shutting down";
      return result;
    }

    const bool crashed_on_probe =
        attempt == 0 and not transport->alive() and mOptions.protocol == "auto" and
        not mOptions.http;
    transport->close();
    result.error = shaken.error;
    result.needs_auth = shaken.needs_auth;
    result.www_authenticate = shaken.www_authenticate;
    if (not crashed_on_probe) break;
    // The process died on the probe: run it again and only say initialize.
    result.error.clear();
  }
  return result;
}

bool Client::ensure_connected(std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mClosing) {
      error = "the MCP client is shutting down";
      return false;
    }
    if (mTransport and mTransport->alive()) return true;
    if (mTransport) error = mTransport->exit_reason();

    // A server that keeps dying is left down rather than restarted forever.
    const Clock::time_point now = Clock::now();
    while (not mRestarts.empty() and now - mRestarts.front() > mOptions.restart_window) {
      mRestarts.pop_front();
    }
    if (static_cast<int>(mRestarts.size()) >= mOptions.max_restarts) {
      error = "the server stopped " + std::to_string(mRestarts.size()) +
              " times in a few minutes and was left down" +
              (error.empty() ? std::string() : " (" + error + ")") +
              "; use /mcp reconnect to try again";
      return false;
    }
    mRestarts.push_back(now);
  }
  const ConnectResult connected = connect();
  if (not connected.ok) {
    error = "could not restart the server: " + connected.error;
    return false;
  }
  return true;
}

// ─────────────────────────────── requests ───────────────────────────────────

Reply Client::request(const std::string& method, util::JsonValue params,
                      std::chrono::milliseconds timeout, const std::string& name,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      const std::atomic<bool>* cancel) {
  if (not params.is_object()) params = util::JsonValue::object();
  for (int attempt = 0;; ++attempt) {
    std::string error;
    if (not ensure_connected(error)) {
      Reply reply;
      reply.error = error;
      return reply;
    }

    std::shared_ptr<Transport> transport;
    Era era = Era::Unknown;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      transport = mTransport;
      era = mEra;
    }

    RequestSpec spec;
    spec.id = next_id();
    spec.method = method;
    spec.params = params;
    // Progress notifications for a tool call extend its deadline; the
    // request's own id is the token, so the transport can match them up.
    const bool progress = method == "tools/call";
    if (era == Era::Modern) {
      spec.params.set("_meta", request_meta(progress, spec.id));
    } else if (progress) {
      spec.params["_meta"].set("progressToken", spec.id);
    }
    spec.name = name;
    spec.headers = headers;
    spec.deadline = Clock::now() + timeout;
    spec.cancel = cancel;
    Reply reply = transport->request(std::move(spec));
    // A legacy HTTP server that forgot our session never ran the request:
    // ensure_connected() starts a new session, and it goes again, once.
    if (reply.session_expired and attempt == 0) continue;
    return reply;
  }
}

bool Client::list_all(const std::string& method, const char* key,
                      std::vector<util::JsonValue>& items, std::string& error) {
  util::JsonValue cursor;
  for (int page = 0; page < kMaxPages; ++page) {
    util::JsonValue params = util::JsonValue::object();
    if (not cursor.is_null()) params.set("cursor", cursor);
    const Reply reply = request(method, std::move(params), mOptions.list_timeout);
    if (not reply.ok) {
      error = reply.error;
      return false;
    }
    if (reply.message.kind != MessageKind::Result) {
      error = method + " failed: " + describe(reply.message.error);
      return false;
    }
    const util::JsonValue& result = reply.message.result;
    for (const util::JsonValue& item : result.get(key).items()) {
      if (items.size() >= kMaxItems) break;
      items.push_back(item);
    }
    if (method == "tools/list") {
      std::lock_guard<std::mutex> lock(mMutex);
      mToolsTtlMs = result.get("ttlMs").as_int(0);
    }
    const std::string& next = result.get("nextCursor").as_string();
    if (next.empty() or items.size() >= kMaxItems) return true;
    cursor = util::JsonValue(next);
  }
  return true;  // a server paging forever gets cut off, not believed
}

bool Client::list_tools(std::vector<ToolInfo>& out, std::string& error) {
  std::vector<util::JsonValue> items;
  if (not list_all("tools/list", "tools", items, error)) return false;
  util::JsonValue array = util::JsonValue::array();
  for (util::JsonValue& item : items) array.push_back(std::move(item));
  out = parse_tools(array);
  return true;
}

bool Client::list_resources(std::vector<ResourceInfo>& out, std::string& error) {
  std::vector<util::JsonValue> items;
  if (not list_all("resources/list", "resources", items, error)) return false;
  util::JsonValue array = util::JsonValue::array();
  for (util::JsonValue& item : items) array.push_back(std::move(item));
  out = parse_resources(array);
  return true;
}

bool Client::list_resource_templates(std::vector<ResourceTemplateInfo>& out,
                                     std::string& error) {
  std::vector<util::JsonValue> items;
  if (not list_all("resources/templates/list", "resourceTemplates", items, error)) {
    return false;
  }
  util::JsonValue array = util::JsonValue::array();
  for (util::JsonValue& item : items) array.push_back(std::move(item));
  out = parse_resource_templates(array);
  return true;
}

bool Client::list_prompts(std::vector<PromptInfo>& out, std::string& error) {
  std::vector<util::JsonValue> items;
  if (not list_all("prompts/list", "prompts", items, error)) return false;
  util::JsonValue array = util::JsonValue::array();
  for (util::JsonValue& item : items) array.push_back(std::move(item));
  out = parse_prompts(array);
  return true;
}

// ─────────────────────── calls and multi-round-trip ─────────────────────────

ElicitationResult Client::elicit(const util::JsonValue& params,
                                 const CallContext& context) {
  ElicitationRequest request;
  request.server = mOptions.server;
  request.agent_label = context.agent_label;
  request.mode = params.get("mode").string_or("form");
  request.message = params.get("message").as_string();
  request.requested_schema = params.get("requestedSchema");
  request.url = params.get("url").as_string();
  if (not mOptions.elicit) return ElicitationResult{"decline", {}};
  return mOptions.elicit(request);
}

bool Client::answer_input_requests(const util::JsonValue& requests,
                                   const CallContext& context,
                                   util::JsonValue& responses,
                                   std::string& error) {
  responses = util::JsonValue::object();
  for (const util::JsonValue::Member& entry : requests.members()) {
    const std::string& method = entry.value.get("method").as_string();
    if (method != "elicitation/create") {
      // Sampling and roots are deprecated and never declared, so a server that
      // asks anyway gets a clear failure rather than a made-up answer.
      error = "the server asked for " + method + ", which m8 does not support";
      return false;
    }
    const ElicitationResult answer = elicit(entry.value.get("params"), context);
    util::JsonValue response = util::JsonValue::object();
    response.set("action", answer.action);
    if (answer.action == "accept" and not answer.content.is_null()) {
      response.set("content", answer.content);
    }
    responses.set(entry.key, std::move(response));
  }
  return true;
}

CallOutcome Client::call_with_input(
    const std::string& method, util::JsonValue params, const std::string& name,
    std::chrono::milliseconds timeout, const CallContext& context,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::atomic<bool>* cancel) {
  CallOutcome outcome;
  bool retried_url_elicitation = false;
  for (int round = 0; round < kMaxInputRounds; ++round) {
    const Reply reply = request(method, params, timeout, name, headers, cancel);
    if (not reply.ok) {
      outcome.error = reply.error;
      outcome.timed_out = reply.timed_out;
      outcome.needs_auth = reply.http_status == 401 or
                           (reply.http_status == 403 and insufficient_scope(reply.www_authenticate));
      outcome.www_authenticate = reply.www_authenticate;
      return outcome;
    }

    if (reply.message.kind == MessageKind::Error) {
      const RpcError& error = reply.message.error;
      // 2025-11-25 servers ask for URL-mode elicitation with an error instead
      // of an InputRequiredResult; consent, then try the call once more.
      if (error.code == kLegacyUrlElicitationRequired and not retried_url_elicitation and
          mOptions.elicit) {
        retried_url_elicitation = true;
        bool accepted = true;
        for (const util::JsonValue& request : error.data.get("elicitations").items()) {
          util::JsonValue url_request = request;
          url_request.set("mode", "url");
          accepted = accepted and elicit(url_request, context).action == "accept";
        }
        if (accepted) continue;
        outcome.error = "the server needs you to visit a URL first, and that was declined";
        return outcome;
      }
      outcome.rpc_code = error.code;
      outcome.error = describe(error);
      return outcome;
    }

    util::JsonValue result = reply.message.result;
    const std::string type = result.get("resultType").string_or("complete");
    if (type == "complete") {
      outcome.ok = true;
      outcome.result = std::move(result);
      return outcome;
    }
    if (type != "input_required") {
      outcome.error = "the server returned an unrecognised resultType '" + type + "'";
      return outcome;
    }

    // Multi round-trip: answer what it asked, echo its state verbatim, retry
    // the same request under a new id.
    params.erase("inputResponses");
    params.erase("requestState");
    const util::JsonValue& requests = result.get("inputRequests");
    if (requests.size() > 0) {
      util::JsonValue responses;
      std::string error;
      if (not answer_input_requests(requests, context, responses, error)) {
        outcome.error = error;
        return outcome;
      }
      params.set("inputResponses", std::move(responses));
    } else {
      // Nothing to ask the user: the server is waiting on something else.
      const auto backoff =
          std::chrono::milliseconds(std::min(2000, 250 << std::min(round, 3)));
      std::this_thread::sleep_for(backoff);
    }
    if (const util::JsonValue* state = result.find("requestState");
        state != nullptr and state->is_string()) {
      params.set("requestState", *state);
    }
  }
  outcome.error = "the server kept asking for more input; gave up after " +
                  std::to_string(kMaxInputRounds) + " rounds";
  return outcome;
}

CallOutcome Client::call_tool(
    const std::string& name, const util::JsonValue& arguments,
    const CallContext& context,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::atomic<bool>* cancel) {
  util::JsonValue params = util::JsonValue::object();
  params.set("name", name);
  params.set("arguments", arguments.is_object() ? arguments : util::JsonValue::object());
  return call_with_input("tools/call", std::move(params), name,
                         mOptions.call_timeout, context, headers, cancel);
}

CallOutcome Client::read_resource(const std::string& uri,
                                  const CallContext& context) {
  util::JsonValue params = util::JsonValue::object();
  params.set("uri", uri);
  CallOutcome outcome = call_with_input("resources/read", std::move(params), uri,
                                        mOptions.read_timeout, context, {}, nullptr);
  if (not outcome.ok and (outcome.rpc_code == kInvalidParams or
                          outcome.rpc_code == kLegacyResourceNotFound)) {
    outcome.error = "no resource " + uri + " (" + outcome.error + ")";
  }
  return outcome;
}

CallOutcome Client::get_prompt(const std::string& name,
                               const util::JsonValue& arguments,
                               const CallContext& context) {
  util::JsonValue params = util::JsonValue::object();
  params.set("name", name);
  if (arguments.size() > 0) params.set("arguments", arguments);
  return call_with_input("prompts/get", std::move(params), name,
                         mOptions.read_timeout, context, {}, nullptr);
}

// ───────────────────────── what the server sends us ─────────────────────────

void Client::on_server_request(const RpcMessage& request, util::JsonValue& result,
                               RpcError& error) {
  if (request.method == "elicitation/create" and mOptions.elicit) {
    const ElicitationResult answer = elicit(request.params, CallContext{});
    result = util::JsonValue::object();
    result.set("action", answer.action);
    if (answer.action == "accept" and not answer.content.is_null()) {
      result.set("content", answer.content);
    }
    return;
  }
  // roots/list and sampling/createMessage are deprecated and never declared.
  error.code = kMethodNotFound;
  error.message = "m8 does not support " + request.method;
}

void Client::on_notification(const RpcMessage& message) {
  if (message.method == "notifications/tools/list_changed" or
      message.method == "notifications/prompts/list_changed" or
      message.method == "notifications/resources/list_changed") {
    if (mOptions.on_list_changed) mOptions.on_list_changed();
  }
}

// ─────────────────────────────── lifetime ───────────────────────────────────

void Client::begin_close() {
  std::shared_ptr<Transport> transport;
  std::shared_ptr<Transport> connecting;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mClosing = true;
    transport = mTransport;
    connecting = mConnecting;
  }
  if (transport) transport->begin_close();
  // Failing its pending requests ends the handshake at once.
  if (connecting) connecting->begin_close();
}

void Client::wait_closed() {
  std::shared_ptr<Transport> transport;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    transport = mTransport;
  }
  if (transport) transport->wait_closed();
}

Era Client::era() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mEra;
}

std::string Client::protocol_version() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mVersion;
}

ServerInfo Client::info() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mInfo;
}

ServerCapabilities Client::capabilities() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mCapabilities;
}

std::string Client::instructions() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mInstructions;
}

bool Client::alive() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mTransport and mTransport->alive();
}

std::string Client::stderr_tail() const {
  std::shared_ptr<Transport> transport;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    transport = mTransport;
  }
  return transport ? transport->stderr_tail() : std::string();
}

int64_t Client::tools_ttl_ms() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mToolsTtlMs;
}

}  // namespace mcp
