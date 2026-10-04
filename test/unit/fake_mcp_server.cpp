// A scripted MCP server for the client tests. It speaks the 2026-07-28
// protocol, the initialize-era protocol, or both, over stdin/stdout — and can
// misbehave on request: start slowly, ignore a probe, crash, print a banner,
// refuse to exit. Flags select the behaviour; see Options.
//
// Tools: echo, add, sleep, fail, ask_name (form elicitation), ask_url (URL
// elicitation), stateful (multi round-trip with only requestState), huge, and
// any number of generated tool_NNN for pagination. One resource, one resource
// template, one prompt.

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include <core/mcp/json_rpc.h>
#include <core/util/json_value.h>

namespace {

using mcp::MessageKind;
using mcp::RpcMessage;
using util::JsonValue;

constexpr const char* kModern = "2026-07-28";

struct Options {
  std::string era = "dual";  // modern|legacy|dual|silent-legacy|modern-only|crash-on-probe
  std::string legacy_version = "2025-11-25";
  int slow_start_ms = 0;
  int tools = 0;
  int page_size = 0;
  bool list_changed = false;
  bool ping_client = false;
  int crash_after = -1;
  bool banner = false;
  bool ignore_eof = false;
  bool ignore_term = false;
  bool stderr_spam = false;
  std::string record;
  std::string instructions;
};

Options parse(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&](const char* prefix) -> std::optional<std::string> {
      const std::string p(prefix);
      if (arg.rfind(p, 0) == 0) return arg.substr(p.size());
      return std::nullopt;
    };
    if (auto v = value("--era=")) options.era = *v;
    else if (auto v = value("--legacy-version=")) options.legacy_version = *v;
    else if (auto v = value("--slow-start-ms=")) options.slow_start_ms = std::atoi(v->c_str());
    else if (auto v = value("--tools=")) options.tools = std::atoi(v->c_str());
    else if (auto v = value("--page-size=")) options.page_size = std::atoi(v->c_str());
    else if (auto v = value("--crash-after=")) options.crash_after = std::atoi(v->c_str());
    else if (auto v = value("--record=")) options.record = *v;
    else if (auto v = value("--instructions=")) options.instructions = *v;
    else if (arg == "--list-changed") options.list_changed = true;
    else if (arg == "--ping-client") options.ping_client = true;
    else if (arg == "--banner") options.banner = true;
    else if (arg == "--ignore-eof") options.ignore_eof = true;
    else if (arg == "--ignore-term") options.ignore_term = true;
    else if (arg == "--stderr-spam") options.stderr_spam = true;
  }
  return options;
}

// Line reader over fd 0 that can wait with a timeout.
struct Lines {
  std::string buffer;
  bool eof = false;

  std::optional<std::string> next(int timeout_ms) {
    while (true) {
      const size_t newline = buffer.find('\n');
      if (newline != std::string::npos) {
        std::string line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        return line;
      }
      if (eof) return std::nullopt;
      pollfd fd{0, POLLIN, 0};
      if (::poll(&fd, 1, timeout_ms) <= 0) return std::nullopt;
      char chunk[65536];
      const ssize_t n = ::read(0, chunk, sizeof(chunk));
      if (n <= 0) {
        eof = true;
        continue;
      }
      buffer.append(chunk, static_cast<size_t>(n));
    }
  }
};

struct Server {
  Options options;
  Lines lines;
  std::deque<std::string> deferred;  // read while waiting for something else
  bool legacy_ready = false;
  int requests = 0;
  int calls = 0;
  int server_ids = 0;
  std::set<std::string> cancelled;
  std::optional<JsonValue> listening;  // the open subscriptions/listen request's id
  std::FILE* record = nullptr;

  explicit Server(Options o) : options(std::move(o)) {
    if (not options.record.empty()) record = std::fopen(options.record.c_str(), "a");
    note("PID " + std::to_string(::getpid()));
  }

  void write(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }

  void note(const std::string& line) {
    if (record == nullptr) return;
    std::fwrite(line.data(), 1, line.size(), record);
    std::fputc('\n', record);
    std::fflush(record);
  }

  std::optional<std::string> next_line(int timeout_ms) {
    if (not deferred.empty()) {
      std::string line = deferred.front();
      deferred.pop_front();
      return line;
    }
    std::optional<std::string> line = lines.next(timeout_ms);
    if (line) note(*line);
    return line;
  }

  bool modern(const RpcMessage& message) const {
    return message.params.get("_meta").contains("io.modelcontextprotocol/protocolVersion");
  }

  JsonValue complete(JsonValue result, bool is_modern) {
    if (is_modern) result.set("resultType", "complete");
    return result;
  }

  JsonValue text_result(const std::string& text, bool is_modern, bool error = false) {
    JsonValue result = JsonValue::object();
    JsonValue block = JsonValue::object();
    block.set("type", "text");
    block.set("text", text);
    result["content"].push_back(block);
    if (error) result.set("isError", true);
    return complete(std::move(result), is_modern);
  }

  JsonValue capabilities() const {
    JsonValue caps = JsonValue::object();
    JsonValue tools = JsonValue::object();
    tools.set("listChanged", true);
    caps.set("tools", tools);
    caps.set("resources", JsonValue::object());
    caps.set("prompts", JsonValue::object());
    return caps;
  }

  JsonValue server_info() const {
    JsonValue info = JsonValue::object();
    info.set("name", "fake-mcp-server");
    info.set("version", "1.0.0");
    return info;
  }

  JsonValue tool(const std::string& name, const std::string& description,
                 const std::string& schema) const {
    JsonValue t = JsonValue::object();
    t.set("name", name);
    t.set("description", description);
    t.set("inputSchema", *JsonValue::parse(schema));
    return t;
  }

  std::vector<JsonValue> all_tools() const {
    std::vector<JsonValue> tools = {
        tool("echo", "Echo a message back",
             R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})"),
        tool("add", "Add two numbers",
             R"({"type":"object","properties":{"a":{"type":"number"},"b":{"type":"number"}},"required":["a","b"]})"),
        tool("sleep", "Sleep for ms milliseconds",
             R"({"type":"object","properties":{"ms":{"type":"integer"}}})"),
        tool("fail", "Always fails", R"({"type":"object"})"),
        tool("ask_name", "Ask the user for their name", R"({"type":"object"})"),
        tool("ask_url", "Send the user to a URL", R"({"type":"object"})"),
        tool("stateful", "Needs a second round trip", R"({"type":"object"})"),
        tool("huge", "Returns a lot of text",
             R"({"type":"object","properties":{"bytes":{"type":"integer"}}})"),
    };
    for (int i = 0; i < options.tools; ++i) {
      char name[32];
      std::snprintf(name, sizeof(name), "tool_%03d", i);
      tools.push_back(tool(name, "Generated tool", R"({"type":"object"})"));
    }
    return tools;
  }

  // Sends a server-to-client request (legacy only) and waits for its answer,
  // deferring anything else that arrives meanwhile.
  std::optional<RpcMessage> ask_client(const std::string& method, const JsonValue& params) {
    const std::string id = "srv-" + std::to_string(++server_ids);
    write(mcp::make_request(JsonValue(id), method, params));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      std::optional<std::string> line = lines.next(100);
      if (not line) {
        if (lines.eof) return std::nullopt;
        continue;
      }
      note(*line);
      RpcMessage message = mcp::parse_message(*line);
      if ((message.kind == MessageKind::Result or message.kind == MessageKind::Error) and
          message.id.as_string() == id) {
        return message;
      }
      deferred.push_back(*line);
    }
    return std::nullopt;
  }

  JsonValue form_request(const std::string& message) const {
    JsonValue params = JsonValue::object();
    params.set("mode", "form");
    params.set("message", message);
    params.set("requestedSchema",
               *JsonValue::parse(R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})"));
    return params;
  }

  // Interruptible: a notifications/cancelled for `id` ends it early.
  bool sleep_for(int ms, const JsonValue& id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
      std::optional<std::string> line = lines.next(20);
      if (not line) continue;
      note(*line);
      RpcMessage message = mcp::parse_message(*line);
      if (message.kind == MessageKind::Notification and
          message.method == "notifications/cancelled" and
          mcp::id_key(message.params.get("requestId")) == mcp::id_key(id)) {
        return false;
      }
      deferred.push_back(*line);
    }
    return true;
  }

  std::optional<JsonValue> call_tool(const RpcMessage& message) {
    const bool is_modern = modern(message);
    const std::string name = message.params.get("name").as_string();
    const JsonValue& args = message.params.get("arguments");
    ++calls;

    if (name == "echo") return text_result(args.get("message").as_string(), is_modern);
    if (name == "add") {
      JsonValue result = text_result(
          std::to_string(args.get("a").as_double() + args.get("b").as_double()), is_modern);
      return result;
    }
    if (name == "fail") return text_result("it failed", is_modern, true);
    if (name == "huge") {
      return text_result(std::string(static_cast<size_t>(args.get("bytes").as_int(1000)), 'x'),
                         is_modern);
    }
    if (name == "sleep") {
      if (not sleep_for(static_cast<int>(args.get("ms").as_int(1000)), message.id)) {
        return std::nullopt;  // cancelled: no response at all
      }
      return text_result("slept", is_modern);
    }
    if (name == "ask_name") {
      if (is_modern) {
        const JsonValue& answer = message.params.get("inputResponses").get("who");
        if (answer.is_null()) {
          JsonValue result = JsonValue::object();
          result.set("resultType", "input_required");
          JsonValue request = JsonValue::object();
          request.set("method", "elicitation/create");
          request.set("params", form_request("What is your name?"));
          result["inputRequests"].set("who", request);
          result.set("requestState", "asked-once");
          return result;
        }
        if (answer.get("action").as_string() != "accept") {
          return text_result("declined", is_modern);
        }
        const std::string state = message.params.get("requestState").as_string();
        return text_result("hello " + answer.get("content").get("name").as_string() +
                               " (state " + state + ")",
                           is_modern);
      }
      std::optional<RpcMessage> answer =
          ask_client("elicitation/create", form_request("What is your name?"));
      if (not answer or answer->kind != MessageKind::Result or
          answer->result.get("action").as_string() != "accept") {
        return text_result("declined", is_modern);
      }
      return text_result("hello " + answer->result.get("content").get("name").as_string(),
                         is_modern);
    }
    if (name == "ask_url") {
      if (is_modern) {
        const JsonValue& answer = message.params.get("inputResponses").get("visit");
        if (answer.is_null()) {
          JsonValue result = JsonValue::object();
          result.set("resultType", "input_required");
          JsonValue params = JsonValue::object();
          params.set("mode", "url");
          params.set("url", "https://example.com/connect");
          params.set("message", "Connect your account");
          JsonValue request = JsonValue::object();
          request.set("method", "elicitation/create");
          request.set("params", params);
          result["inputRequests"].set("visit", request);
          return result;
        }
        return text_result(answer.get("action").as_string() == "accept" ? "visited" : "declined",
                           is_modern);
      }
      return text_result("not supported", is_modern, true);
    }
    if (name == "stateful") {
      const std::string state = message.params.get("requestState").as_string();
      if (state.empty()) {
        JsonValue result = JsonValue::object();
        result.set("resultType", "input_required");
        result.set("requestState", "s1");
        return result;
      }
      if (state != "s1") return text_result("bad state " + state, is_modern, true);
      return text_result("state ok", is_modern);
    }
    if (name.rfind("tool_", 0) == 0) return text_result(name, is_modern);
    return std::nullopt;
  }

  void respond(const RpcMessage& message, const JsonValue& result) {
    write(mcp::make_result(message.id, result));
  }

  void error(const RpcMessage& message, int64_t code, const std::string& text) {
    write(mcp::make_error(message.id, code, text));
  }

  void handle(const std::string& line) {
    const RpcMessage message = mcp::parse_message(line);
    if (message.kind == MessageKind::Notification) {
      if (message.method == "notifications/initialized") {
        legacy_ready = true;
        if (options.ping_client) {
          std::optional<RpcMessage> pong = ask_client("ping", JsonValue());
          note(pong ? "PING-ANSWERED" : "PING-UNANSWERED");
        }
      }
      if (message.method == "notifications/cancelled" and listening and
          mcp::id_key(message.params.get("requestId")) == mcp::id_key(*listening)) {
        listening.reset();
        note("SUBSCRIPTION-CANCELLED\n");
      }
      return;
    }
    if (message.kind != MessageKind::Request) return;

    ++requests;
    if (options.crash_after >= 0 and requests > options.crash_after) {
      std::fprintf(stderr, "fake server crashing on purpose\n");
      std::fflush(stderr);
      std::_Exit(3);
    }

    const bool is_modern = modern(message);
    const std::string& method = message.method;
    const std::string& era = options.era;

    if (method == "server/discover") {
      if (era == "crash-on-probe") std::_Exit(1);
      if (era == "silent-legacy") return;
      if (era == "legacy") return error(message, mcp::kMethodNotFound, "Method not found");
      const std::string requested =
          message.params.get("_meta").get("io.modelcontextprotocol/protocolVersion").as_string();
      JsonValue supported = JsonValue::array();
      supported.push_back(kModern);
      if (era == "dual") supported.push_back(options.legacy_version);
      if (requested != kModern) {
        JsonValue data = JsonValue::object();
        data.set("supported", supported);
        data.set("requested", requested);
        JsonValue err = JsonValue::object();
        err.set("code", mcp::kUnsupportedProtocolVersion);
        err.set("message", "Unsupported protocol version");
        err.set("data", data);
        JsonValue out = JsonValue::object();
        out.set("jsonrpc", "2.0");
        out.set("id", message.id);
        out.set("error", err);
        return write(out.dump());
      }
      JsonValue result = JsonValue::object();
      result.set("resultType", "complete");
      result.set("supportedVersions", supported);
      result.set("capabilities", capabilities());
      result["_meta"].set("io.modelcontextprotocol/serverInfo", server_info());
      if (not options.instructions.empty()) result.set("instructions", options.instructions);
      return respond(message, result);
    }

    if (method == "initialize") {
      if (era == "modern-only" or era == "modern") {
        return error(message, mcp::kMethodNotFound,
                     "this server only speaks protocol version 2026-07-28");
      }
      JsonValue result = JsonValue::object();
      result.set("protocolVersion", options.legacy_version);
      result.set("capabilities", capabilities());
      result.set("serverInfo", server_info());
      if (not options.instructions.empty()) result.set("instructions", options.instructions);
      return respond(message, result);
    }

    if (not is_modern and not legacy_ready) {
      if (era == "silent-legacy") return;
      if (era != "dual" and era != "legacy" and era != "crash-on-probe") {
        return error(message, mcp::kInvalidParams, "missing _meta");
      }
      return error(message, mcp::kInvalidRequest, "Server not initialized");
    }

    if (method == "tools/list") {
      const std::vector<JsonValue> tools = all_tools();
      size_t start = 0;
      if (const std::string& cursor = message.params.get("cursor").as_string(); not cursor.empty()) {
        start = static_cast<size_t>(std::atoi(cursor.c_str()));
      }
      const size_t page = options.page_size > 0 ? static_cast<size_t>(options.page_size) : tools.size();
      JsonValue result = JsonValue::object();
      JsonValue list = JsonValue::array();
      for (size_t i = start; i < tools.size() and i < start + page; ++i) list.push_back(tools[i]);
      result.set("tools", list);
      if (start + page < tools.size()) result.set("nextCursor", std::to_string(start + page));
      if (is_modern) {
        result.set("ttlMs", 300000);
        result.set("cacheScope", "public");
      }
      return respond(message, complete(result, is_modern));
    }

    if (method == "subscriptions/listen") {
      if (not is_modern) return error(message, mcp::kMethodNotFound, "Method not found");
      // Acknowledged first, then held open: what it asked for that this
      // server does, list changes, arrive on it until it is cancelled.
      listening = message.id;
      JsonValue params = JsonValue::object();
      params["_meta"].set("io.modelcontextprotocol/subscriptionId", message.id);
      JsonValue granted = JsonValue::object();
      if (message.params.get("notifications").get("toolsListChanged").as_bool()) {
        granted.set("toolsListChanged", true);
      }
      params.set("notifications", granted);
      write(mcp::make_notification("notifications/subscriptions/acknowledged", params));
      note("SUBSCRIBED\n");
      return;
    }

    if (method == "tools/call") {
      std::optional<JsonValue> result = call_tool(message);
      if (not result) {
        if (message.params.get("name").as_string() == "sleep") return;  // cancelled
        return error(message, mcp::kInvalidParams,
                     "Unknown tool: " + message.params.get("name").as_string());
      }
      respond(message, *result);
      if (options.list_changed and calls == 1) {
        if (not is_modern) {
          // Before 2026-07-28 servers announced changes unasked.
          write(mcp::make_notification("notifications/tools/list_changed", JsonValue()));
        } else if (listening) {
          JsonValue params = JsonValue::object();
          params["_meta"].set("io.modelcontextprotocol/subscriptionId", *listening);
          write(mcp::make_notification("notifications/tools/list_changed", params));
        }
      }
      return;
    }

    if (method == "resources/list") {
      JsonValue result = JsonValue::object();
      result["resources"].push_back(*JsonValue::parse(
          R"({"uri":"mem://greeting","name":"greeting","mimeType":"text/plain","description":"A greeting"})"));
      return respond(message, complete(result, is_modern));
    }
    if (method == "resources/templates/list") {
      JsonValue result = JsonValue::object();
      result["resourceTemplates"].push_back(
          *JsonValue::parse(R"({"uriTemplate":"mem://items/{id}","name":"item"})"));
      return respond(message, complete(result, is_modern));
    }
    if (method == "resources/read") {
      const std::string uri = message.params.get("uri").as_string();
      std::string text;
      if (uri == "mem://greeting") text = "hello";
      if (uri.rfind("mem://items/", 0) == 0) text = "item " + uri.substr(12);
      if (text.empty()) {
        return error(message, is_modern ? mcp::kInvalidParams : mcp::kLegacyResourceNotFound,
                     "Resource not found");
      }
      JsonValue entry = JsonValue::object();
      entry.set("uri", uri);
      entry.set("mimeType", "text/plain");
      entry.set("text", text);
      JsonValue result = JsonValue::object();
      result["contents"].push_back(entry);
      return respond(message, complete(result, is_modern));
    }
    if (method == "prompts/list") {
      JsonValue result = JsonValue::object();
      result["prompts"].push_back(*JsonValue::parse(
          R"({"name":"review","description":"Review code","arguments":[{"name":"code","required":true}]})"));
      return respond(message, complete(result, is_modern));
    }
    if (method == "prompts/get") {
      JsonValue content = JsonValue::object();
      content.set("type", "text");
      content.set("text", "Review this code:\n" +
                              message.params.get("arguments").get("code").as_string());
      JsonValue entry = JsonValue::object();
      entry.set("role", "user");
      entry.set("content", content);
      JsonValue result = JsonValue::object();
      result["messages"].push_back(entry);
      return respond(message, complete(result, is_modern));
    }
    error(message, mcp::kMethodNotFound, "Method not found: " + method);
  }

  void run() {
    while (true) {
      std::optional<std::string> line = next_line(200);
      if (not line) {
        if (lines.eof and deferred.empty()) break;
        continue;
      }
      if (line->empty()) continue;
      handle(*line);
    }
    if (options.ignore_eof) {
      // Refuses the polite shutdown: only a signal ends it.
      while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  // For a test that needs the same configuration to fail on a second run.
  if (const char* exit_now = std::getenv("FAKE_MCP_EXIT_AT_START");
      exit_now != nullptr and std::string(exit_now) == "1") {
    return 4;
  }
  const Options options = parse(argc, argv);
  if (options.ignore_term) ::signal(SIGTERM, SIG_IGN);
  if (options.stderr_spam) {
    for (int i = 0; i < 200; ++i) std::fprintf(stderr, "spam line %d\n", i);
    std::fflush(stderr);
  }
  if (options.slow_start_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(options.slow_start_ms));
  }
  if (options.banner) {
    std::printf("fake MCP server starting up (this line is not JSON)\n");
    std::fflush(stdout);
  }
  Server server(options);
  server.run();
  return 0;
}
