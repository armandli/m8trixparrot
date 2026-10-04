// Streamable HTTP in both of its shapes, against scripted servers on loopback:
// 2026-07-28 (stateless; the body's metadata mirrored into headers; closing
// the stream cancels) and 2025-03-26 to 2025-11-25 (sessions, server requests
// on a stream, explicit cancellation, resumption). Plus the client's era
// detection over HTTP, which falls back to `initialize` only on a 4xx that is
// not a modern error, and the registry's HeaderMismatch retry.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <core/mcp/client.h>
#include <core/mcp/http_transport.h>
#include <core/mcp/json_rpc.h>
#include <core/mcp/registry.h>
#include <core/util/json_value.h>

#include <http_test_server.h>

namespace mcp {
namespace {

using namespace std::chrono_literals;
using m8test::HttpReply;
using m8test::HttpRequest;
using m8test::HttpTestServer;
using util::JsonValue;

constexpr auto npos = std::string::npos;

JsonValue json(std::string_view text) {
  std::optional<JsonValue> value = JsonValue::parse(text);
  EXPECT_TRUE(value.has_value()) << text;
  return value.value_or(JsonValue());
}

HttpReply json_reply(int status, const std::string& body) {
  HttpReply reply;
  reply.status = status;
  reply.headers = {{"Content-Type", "application/json"}};
  reply.body = body;
  return reply;
}

HttpReply sse_reply(std::vector<std::string> events, std::chrono::milliseconds delay = 0ms) {
  HttpReply reply;
  reply.headers = {{"Content-Type", "text/event-stream"}, {"Cache-Control", "no-cache"}};
  reply.chunks = std::move(events);
  reply.delay = delay;
  return reply;
}

// One SSE event; a payload with line breaks takes one `data:` line per line,
// as a real server writes it.
std::string event(const std::string& data, const std::string& id = "") {
  std::string out = id.empty() ? std::string() : "id: " + id + "\n";
  size_t at = 0;
  while (true) {
    const size_t newline = data.find('\n', at);
    out += "data: " + data.substr(at, newline == std::string::npos ? std::string::npos : newline - at) + "\n";
    if (newline == std::string::npos) break;
    at = newline + 1;
  }
  return out + "\n";
}

// A stream that only sends keep-alives, until the client goes away.
HttpReply endless_stream() {
  HttpReply reply = sse_reply({});
  auto sent = std::make_shared<int>(0);
  reply.stream = [sent]() -> std::optional<std::string> {
    if (++*sent > 200) return std::nullopt;  // 10 s: a test that forgot to stop
    std::this_thread::sleep_for(50ms);
    return std::string(":\n\n");
  };
  return reply;
}

std::string error_body(const JsonValue& id, int64_t code, const std::string& message,
                       const JsonValue& data = JsonValue()) {
  JsonValue error = JsonValue::object();
  error.set("code", code);
  error.set("message", message);
  if (not data.is_null()) error.set("data", data);
  JsonValue out = JsonValue::object();
  out.set("jsonrpc", "2.0");
  out.set("id", id);
  out.set("error", error);
  return out.dump();
}

JsonValue text_result(const std::string& text, bool modern) {
  JsonValue result = JsonValue::object();
  if (modern) result.set("resultType", "complete");
  JsonValue block = JsonValue::object();
  block.set("type", "text");
  block.set("text", text);
  result["content"].push_back(block);
  return result;
}

// An MCP server over HTTP for one test: the protocol basics for one era, and
// a hook a test uses to answer something differently.
struct FakeHttpServer {
  enum struct Kind { Modern, Stateful, Stateless };

  explicit FakeHttpServer(Kind kind)
      : kind(kind), http([this](const HttpRequest& request) { return handle(request); }) {}

  using Hook = std::function<std::optional<HttpReply>(const HttpRequest&, const RpcMessage&)>;

  void set_hook(Hook hook) {
    std::lock_guard<std::mutex> lock(mutex);
    mHook = std::move(hook);
  }
  void set_session(std::string session) {
    std::lock_guard<std::mutex> lock(mutex);
    mSession = std::move(session);
  }
  void set_tools(JsonValue tools) {
    std::lock_guard<std::mutex> lock(mutex);
    mTools = std::move(tools);
  }

  std::string url() const { return http.url("/mcp"); }

  // Requests whose JSON-RPC method (or HTTP method, for GET and DELETE) is `method`.
  std::vector<HttpRequest> requests(const std::string& method) const {
    std::vector<HttpRequest> out;
    for (const HttpRequest& request : http.requests()) {
      const std::string name =
          request.method == "POST" ? parse_message(request.body).method : request.method;
      if (name == method) out.push_back(request);
    }
    return out;
  }

  HttpReply answer(const RpcMessage& message, const JsonValue& result) const {
    const std::string body = make_result(message.id, result);
    return sse ? sse_reply({event(body)}) : json_reply(200, body);
  }

  HttpReply handle(const HttpRequest& request) {
    const RpcMessage message = parse_message(request.body);
    Hook hook;
    std::string session;
    JsonValue tools;
    {
      std::lock_guard<std::mutex> lock(mutex);
      hook = mHook;
      session = mSession;
      tools = mTools;
    }
    if (hook) {
      if (std::optional<HttpReply> reply = hook(request, message)) return *reply;
    }
    if (request.method == "DELETE") return HttpReply{};
    if (request.method == "GET") return HttpReply{405};
    if (message.kind != MessageKind::Request) {
      HttpReply accepted;
      accepted.status = 202;
      return accepted;
    }

    const bool modern = kind == Kind::Modern;
    const std::string& method = message.method;
    if (modern) {
      if (method == "server/discover") {
        JsonValue result = json(R"({"resultType":"complete","supportedVersions":["2026-07-28"],
          "capabilities":{"tools":{}},
          "_meta":{"io.modelcontextprotocol/serverInfo":{"name":"http-fake","version":"1"}}})");
        return answer(message, result);
      }
    } else if (method == "initialize") {
      ++initializes;
      HttpReply reply = answer(message, json(R"({"protocolVersion":"2025-11-25",
        "capabilities":{"tools":{}},"serverInfo":{"name":"http-fake","version":"1"}})"));
      if (kind == Kind::Stateful) reply.headers.emplace_back("Mcp-Session-Id", session);
      return reply;
    } else if (kind == Kind::Stateful) {
      const std::string sent = request.header("Mcp-Session-Id");
      if (sent.empty()) {
        return json_reply(400, error_body(JsonValue(), -32000,
                                          "Bad Request: Mcp-Session-Id header is required"));
      }
      if (sent != session) {
        return json_reply(404, error_body(JsonValue(), -32001, "Session not found"));
      }
    }

    if (method == "tools/list") {
      JsonValue result = JsonValue::object();
      if (modern) result.set("resultType", "complete");
      result.set("tools", tools);
      return answer(message, result);
    }
    if (method == "tools/call") {
      const std::string text =
          message.params.get("arguments").get("message").string_or("(nothing)");
      return answer(message, text_result("echo: " + text, modern));
    }
    return json_reply(modern ? 404 : 200, error_body(message.id, kMethodNotFound, "Method not found"));
  }

  const Kind kind;
  bool sse = false;
  std::atomic<int> initializes{0};

  mutable std::mutex mutex;
  Hook mHook;
  std::string mSession = "session-1";
  JsonValue mTools = json(R"([{"name":"echo","description":"Echo a message",
    "inputSchema":{"type":"object","properties":{"message":{"type":"string"}}}}])");

  HttpTestServer http;  // last: its threads call handle()
};

ClientOptions client_options(const std::string& url) {
  ClientOptions options;
  options.server = "web";
  options.http = true;
  options.startup_timeout = 5s;
  options.list_timeout = 5s;
  options.call_timeout = 5s;
  options.read_timeout = 5s;
  options.make_transport = [url](TransportHandlers handlers) -> std::unique_ptr<Transport> {
    HttpConfig config;
    config.server = "web";
    config.url = url;
    return std::make_unique<HttpTransport>(config, std::move(handlers));
  };
  return options;
}

RequestSpec call_spec(int64_t id, const std::string& message,
                      std::chrono::milliseconds timeout = 5000ms) {
  RequestSpec spec;
  spec.id = JsonValue(id);
  spec.method = "tools/call";
  spec.name = "echo";
  spec.params = JsonValue::object();
  spec.params.set("name", "echo");
  spec.params["arguments"].set("message", message);
  spec.deadline = Clock::now() + timeout;
  return spec;
}

std::string text_of(const JsonValue& result) {
  const auto& content = result.get("content").items();
  return content.empty() ? std::string() : content.front().get("text").as_string();
}

// ─────────────────────────────── 2026-07-28 ─────────────────────────────────

TEST(McpHttpTest, AModernServerIsAskedWithItsMetadataMirroredIntoHeaders) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  Client client(client_options(server.url()));
  const ConnectResult connected = client.connect();
  ASSERT_TRUE(connected.ok) << connected.error;
  EXPECT_EQ(client.era(), Era::Modern);

  std::vector<ToolInfo> tools;
  std::string error;
  ASSERT_TRUE(client.list_tools(tools, error)) << error;
  ASSERT_EQ(tools.size(), 1u);

  const CallOutcome outcome =
      client.call_tool("echo", json(R"({"message":"hi"})"), CallContext{});
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_EQ(text_of(outcome.result), "echo: hi");

  const std::vector<HttpRequest> calls = server.requests("tools/call");
  ASSERT_EQ(calls.size(), 1u);
  const HttpRequest& call = calls.front();
  EXPECT_EQ(call.header("MCP-Protocol-Version"), "2026-07-28");
  EXPECT_EQ(call.header("Mcp-Method"), "tools/call");
  EXPECT_EQ(call.header("Mcp-Name"), "echo");
  EXPECT_EQ(call.header("Content-Type"), "application/json");
  EXPECT_NE(call.header("Accept").find("application/json"), npos);
  EXPECT_NE(call.header("Accept").find("text/event-stream"), npos);
  EXPECT_EQ(call.header("Mcp-Session-Id"), "");
  EXPECT_EQ(parse_message(call.body)
                .params.get("_meta")
                .get("io.modelcontextprotocol/protocolVersion")
                .as_string(),
            "2026-07-28");
  EXPECT_EQ(server.requests("tools/list").front().header("Mcp-Name"), "");
  EXPECT_TRUE(server.requests("initialize").empty());

  client.close();
  // Nothing to end: 2026-07-28 has no sessions.
  EXPECT_TRUE(server.requests("DELETE").empty());
}

TEST(McpHttpTest, AnSseAnswerCarriesNotificationsBeforeTheResponse) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  server.set_hook([](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "tools/call") return std::nullopt;
    return sse_reply({
        event(R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":"working"}})"),
        event(R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":7,"progress":1}})"),
        ": keep-alive\n\n",
        event(make_result(message.id, text_result("done", true)), "e-3"),
    });
  });

  std::vector<std::string> seen;
  std::mutex seen_mutex;
  TransportHandlers handlers;
  handlers.on_notification = [&](const RpcMessage& message) {
    std::lock_guard<std::mutex> lock(seen_mutex);
    seen.push_back(message.method);
  };
  HttpConfig config;
  config.url = server.url();
  HttpTransport transport(config, handlers);
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  transport.set_protocol(Era::Modern, kModernVersion);

  const Reply reply = transport.request(call_spec(7, "x"));
  ASSERT_TRUE(reply.ok) << reply.error;
  EXPECT_EQ(reply.message.kind, MessageKind::Result);
  EXPECT_EQ(text_of(reply.message.result), "done");
  // Progress is the transport's own business; the rest goes to the client.
  EXPECT_EQ(seen, std::vector<std::string>{"notifications/message"});
}

TEST(McpHttpTest, ProgressKeepsALongCallAliveAndSilenceTimesItOut) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  server.set_hook([](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "tools/call") return std::nullopt;
    const bool with_progress = message.params.get("arguments").get("message").as_string() == "progress";
    std::vector<std::string> events;
    for (int i = 0; i < 8; ++i) {
      events.push_back(with_progress
                           ? event(R"({"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":)" +
                                   message.id.dump() + R"(,"progress":)" + std::to_string(i) + "}}")
                           : std::string(":\n\n"));
    }
    events.push_back(event(make_result(message.id, text_result("finally", true))));
    return sse_reply(events, 100ms);
  });

  HttpConfig config;
  config.url = server.url();
  HttpTransport transport(config, TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  transport.set_protocol(Era::Modern, kModernVersion);

  // 900 ms of work against a 300 ms timeout: progress every 100 ms keeps it going.
  const Reply kept = transport.request(call_spec(1, "progress", 300ms));
  ASSERT_TRUE(kept.ok) << kept.error;
  EXPECT_EQ(text_of(kept.message.result), "finally");

  // Keep-alive comments are not progress.
  const auto started = Clock::now();
  const Reply silent = transport.request(call_spec(2, "quiet", 300ms));
  EXPECT_FALSE(silent.ok);
  EXPECT_TRUE(silent.timed_out);
  EXPECT_LT(Clock::now() - started, 800ms);
}

TEST(McpHttpTest, ClosingTheStreamIsHowAModernRequestIsCancelled) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  server.set_hook([](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "tools/call") return std::nullopt;
    return endless_stream();
  });

  HttpConfig config;
  config.url = server.url();
  HttpTransport transport(config, TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  transport.set_protocol(Era::Modern, kModernVersion);

  std::atomic<bool> cancel{false};
  RequestSpec spec = call_spec(1, "x", 10000ms);
  spec.cancel = &cancel;
  std::thread canceller([&] {
    std::this_thread::sleep_for(200ms);
    cancel.store(true);
  });
  const auto started = Clock::now();
  const Reply reply = transport.request(spec);
  canceller.join();
  EXPECT_TRUE(reply.cancelled);
  EXPECT_LT(Clock::now() - started, 1500ms);

  // The server saw its stream go away — and no notifications/cancelled.
  for (int i = 0; i < 40 and server.http.aborted_streams() == 0; ++i) std::this_thread::sleep_for(50ms);
  EXPECT_GE(server.http.aborted_streams(), 1);
  EXPECT_TRUE(server.requests("notifications/cancelled").empty());
}

TEST(McpHttpTest, NamesAndParametersThatAreNotPlainAsciiGoInTheSentinelForm) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  HttpConfig config;
  config.url = server.url();
  HttpTransport transport(config, TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  transport.set_protocol(Era::Modern, kModernVersion);

  RequestSpec spec = call_spec(1, "x");
  spec.name = "caf\xc3\xa9";
  spec.headers = {{"Mcp-Param-Region", "us-west1"}, {"Mcp-Param-Evil", "a\r\nX-Injected: 1"}};
  transport.request(spec);

  const std::vector<HttpRequest> calls = server.requests("tools/call");
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].header("Mcp-Name"), "=?base64?Y2Fmw6k=?=");
  EXPECT_EQ(calls[0].header("Mcp-Param-Region"), "us-west1");
  // A value that would split the header line is never sent.
  EXPECT_EQ(calls[0].header("Mcp-Param-Evil"), "");
  EXPECT_EQ(calls[0].header("X-Injected"), "");
}

TEST(McpHttpTest, ConfiguredHeadersAndCredentialsGoOnEveryRequest) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  HttpConfig config;
  config.url = server.url();
  config.headers = {{"X-Api-Key", "k-123"}, {"Mcp-Method", "spoofed"}};
  std::atomic<int> asked{0};
  config.authorization = [&] {
    ++asked;
    return std::string("Bearer token-1");
  };
  HttpTransport transport(config, TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  transport.set_protocol(Era::Modern, kModernVersion);
  transport.request(call_spec(1, "x"));

  const HttpRequest call = server.requests("tools/call").front();
  EXPECT_EQ(call.header("X-Api-Key"), "k-123");
  EXPECT_EQ(call.header("Authorization"), "Bearer token-1");
  // A configured header cannot overwrite what the body says.
  EXPECT_EQ(call.header("Mcp-Method"), "tools/call");
  EXPECT_EQ(asked.load(), 1);
  EXPECT_NE(call.header("User-Agent").find("m8/"), npos);

  // A static Authorization in the config wins over the provider.
  HttpConfig fixed = config;
  fixed.headers = {{"Authorization", "Bearer static"}};
  HttpTransport second(fixed, TransportHandlers{});
  ASSERT_TRUE(second.start(error)) << error;
  second.set_protocol(Era::Modern, kModernVersion);
  second.request(call_spec(2, "x"));
  EXPECT_EQ(server.requests("tools/call").back().header("Authorization"), "Bearer static");
  EXPECT_EQ(asked.load(), 1);
}

TEST(McpHttpTest, ManyCallersShareOneTransport) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  Client client(client_options(server.url()));
  ASSERT_TRUE(client.connect().ok);

  std::atomic<int> succeeded{0};
  std::vector<std::thread> callers;
  for (int t = 0; t < 8; ++t) {
    callers.emplace_back([&, t] {
      for (int i = 0; i < 5; ++i) {
        const std::string text = std::to_string(t) + "-" + std::to_string(i);
        const CallOutcome outcome =
            client.call_tool("echo", json(R"({"message":")" + text + R"("})"), CallContext{});
        if (outcome.ok and text_of(outcome.result) == "echo: " + text) ++succeeded;
      }
    });
  }
  for (std::thread& caller : callers) caller.join();
  EXPECT_EQ(succeeded.load(), 40);
}

TEST(McpHttpTest, CloseEndsARequestInFlightPromptly) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  server.set_hook([](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "tools/call") return std::nullopt;
    return endless_stream();
  });
  HttpConfig config;
  config.url = server.url();
  HttpTransport transport(config, TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  transport.set_protocol(Era::Modern, kModernVersion);

  Reply reply;
  std::thread caller([&] { reply = transport.request(call_spec(1, "x", 30000ms)); });
  std::this_thread::sleep_for(200ms);
  const auto started = Clock::now();
  transport.close();
  caller.join();
  EXPECT_LT(Clock::now() - started, 1000ms);
  EXPECT_FALSE(reply.ok);
  EXPECT_TRUE(reply.cancelled);
  EXPECT_FALSE(transport.alive());
}

TEST(McpHttpTest, AnAnswerPastTheSizeLimitIsRefused) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  server.set_hook([](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "tools/call") return std::nullopt;
    return json_reply(200, make_result(message.id, text_result(std::string(4096, 'x'), true)));
  });
  HttpConfig config;
  config.url = server.url();
  config.max_body_bytes = 1024;
  HttpTransport transport(config, TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  transport.set_protocol(Era::Modern, kModernVersion);
  const Reply reply = transport.request(call_spec(1, "x"));
  EXPECT_FALSE(reply.ok);
  EXPECT_NE(reply.error.find("larger than"), npos) << reply.error;
}

TEST(McpHttpTest, BadUrlsAreRefusedBeforeAnythingIsSent) {
  std::string error;
  for (const std::string url : {"ftp://example.com/mcp", "not a url",
                                "https://user:pw@example.com/mcp"}) {
    HttpConfig config;
    config.url = url;
    HttpTransport transport(config, TransportHandlers{});
    EXPECT_FALSE(transport.start(error)) << url;
    EXPECT_FALSE(error.empty());
  }
}

// ──────────────────────────── era detection ─────────────────────────────────

TEST(McpHttpTest, AServerErrorIsNotMistakenForAnOlderServer) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateless);
  server.set_hook([](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "server/discover") return std::nullopt;
    HttpReply reply;
    reply.status = 500;
    reply.headers = {{"Content-Type", "text/plain"}};
    reply.body = "upstream is down";
    return reply;
  });
  Client client(client_options(server.url()));
  const ConnectResult connected = client.connect();
  EXPECT_FALSE(connected.ok);
  EXPECT_NE(connected.error.find("500"), npos) << connected.error;
  EXPECT_TRUE(server.requests("initialize").empty());
}

TEST(McpHttpTest, AnUnreachableServerIsNotMistakenForAnOlderServer) {
  // A port nothing listens on: bind one, note it, close it.
  int port = 0;
  { HttpTestServer closed([](const HttpRequest&) { return HttpReply{}; }); port = closed.port(); }
  ClientOptions options = client_options("http://127.0.0.1:" + std::to_string(port) + "/mcp");
  options.startup_timeout = 2s;
  Client client(options);
  const ConnectResult connected = client.connect();
  EXPECT_FALSE(connected.ok);
  EXPECT_NE(connected.error.find("could not reach"), npos) << connected.error;
}

TEST(McpHttpTest, A404WithoutAModernErrorMeansAnOlderServer) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateless);
  server.set_hook([](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "server/discover") return std::nullopt;
    HttpReply reply;
    reply.status = 404;
    reply.headers = {{"Content-Type", "text/html"}};
    reply.body = "<h1>Not Found</h1>";
    return reply;
  });
  Client client(client_options(server.url()));
  const ConnectResult connected = client.connect();
  ASSERT_TRUE(connected.ok) << connected.error;
  EXPECT_EQ(client.era(), Era::Legacy);
  EXPECT_EQ(client.protocol_version(), "2025-11-25");
  EXPECT_EQ(server.initializes.load(), 1);
}

TEST(McpHttpTest, AModernErrorIsNotMistakenForAnOlderServer) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  server.set_hook([](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "server/discover") return std::nullopt;
    return json_reply(400, error_body(message.id, kUnsupportedProtocolVersion,
                                      "Unsupported protocol version",
                                      json(R"({"supported":["2027-01-01"],"requested":"2026-07-28"})")));
  });
  Client client(client_options(server.url()));
  const ConnectResult connected = client.connect();
  EXPECT_FALSE(connected.ok);
  EXPECT_NE(connected.error.find("2027-01-01"), npos) << connected.error;
  EXPECT_TRUE(server.requests("initialize").empty());
}

TEST(McpHttpTest, A401AsksForALoginAndKeepsTheChallenge) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  server.set_hook([](const HttpRequest&, const RpcMessage&) -> std::optional<HttpReply> {
    HttpReply reply = json_reply(401, error_body(JsonValue(), -32001, "Unauthorized"));
    reply.headers.emplace_back("WWW-Authenticate",
                               R"(Bearer resource_metadata="http://127.0.0.1/.well-known/oauth-protected-resource/mcp", scope="files:read")");
    return reply;
  });
  Client client(client_options(server.url()));
  const ConnectResult connected = client.connect();
  EXPECT_FALSE(connected.ok);
  EXPECT_TRUE(connected.needs_auth);
  EXPECT_NE(connected.www_authenticate.find("resource_metadata="), npos)
      << connected.www_authenticate;
  EXPECT_TRUE(server.requests("initialize").empty());
}

TEST(McpHttpTest, RedirectsAreReportedRatherThanFollowed) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  server.set_hook([](const HttpRequest&, const RpcMessage&) -> std::optional<HttpReply> {
    HttpReply reply;
    reply.status = 307;
    reply.headers = {{"Location", "http://127.0.0.1:9/elsewhere"}};
    return reply;
  });
  HttpConfig config;
  config.url = server.url();
  HttpTransport transport(config, TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  transport.set_protocol(Era::Modern, kModernVersion);
  const Reply reply = transport.request(call_spec(1, "x"));
  EXPECT_FALSE(reply.ok);
  EXPECT_NE(reply.error.find("redirect"), npos) << reply.error;
  EXPECT_NE(reply.error.find("127.0.0.1:9/elsewhere"), npos) << reply.error;
  EXPECT_EQ(server.http.request_count(), 1u);
}

// ────────────────────────── 2025-03-26 .. 2025-11-25 ────────────────────────

TEST(McpHttpTest, AnOlderServerGetsASessionCarriedOnEveryLaterRequest) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateful);
  Client client(client_options(server.url()));
  const ConnectResult connected = client.connect();
  ASSERT_TRUE(connected.ok) << connected.error;
  EXPECT_EQ(client.era(), Era::Legacy);

  std::vector<ToolInfo> tools;
  std::string error;
  ASSERT_TRUE(client.list_tools(tools, error)) << error;
  const CallOutcome outcome =
      client.call_tool("echo", json(R"({"message":"old"})"), CallContext{});
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_EQ(text_of(outcome.result), "echo: old");

  const HttpRequest initialize = server.requests("initialize").front();
  EXPECT_EQ(initialize.header("Mcp-Session-Id"), "");
  EXPECT_EQ(initialize.header("MCP-Protocol-Version"), "");
  const std::vector<HttpRequest> initialized = server.requests("notifications/initialized");
  ASSERT_EQ(initialized.size(), 1u);
  EXPECT_EQ(initialized[0].header("Mcp-Session-Id"), "session-1");
  const HttpRequest list = server.requests("tools/list").front();
  EXPECT_EQ(list.header("Mcp-Session-Id"), "session-1");
  EXPECT_EQ(list.header("MCP-Protocol-Version"), "2025-11-25");
  EXPECT_EQ(list.header("Mcp-Method"), "");

  client.close();
  const std::vector<HttpRequest> deletes = server.requests("DELETE");
  ASSERT_EQ(deletes.size(), 1u);
  EXPECT_EQ(deletes[0].header("Mcp-Session-Id"), "session-1");
}

TEST(McpHttpTest, AForgottenSessionIsRenewedAndTheRequestSentAgain) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateful);
  Client client(client_options(server.url()));
  ASSERT_TRUE(client.connect().ok);
  std::vector<ToolInfo> tools;
  std::string error;
  ASSERT_TRUE(client.list_tools(tools, error)) << error;

  server.set_session("session-2");  // the server restarted
  const CallOutcome outcome =
      client.call_tool("echo", json(R"({"message":"again"})"), CallContext{});
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_EQ(text_of(outcome.result), "echo: again");
  EXPECT_EQ(server.initializes.load(), 2);

  const std::vector<HttpRequest> calls = server.requests("tools/call");
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].header("Mcp-Session-Id"), "session-1");
  EXPECT_EQ(calls[1].header("Mcp-Session-Id"), "session-2");
  // A fresh id for the resend.
  EXPECT_NE(parse_message(calls[0].body).id.dump(), parse_message(calls[1].body).id.dump());
}

TEST(McpHttpTest, AnOlderServersRequestsOnAStreamAreAnswered) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateful);
  std::mutex mutex;
  std::condition_variable answered_cv;
  std::vector<RpcMessage> answers;
  server.set_hook([&](const HttpRequest& request, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.kind == MessageKind::Result or message.kind == MessageKind::Error) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        answers.push_back(message);
      }
      answered_cv.notify_all();
      HttpReply accepted;
      accepted.status = 202;
      return accepted;
    }
    if (message.method != "tools/call" or request.header("Mcp-Session-Id").empty()) {
      return std::nullopt;
    }
    HttpReply reply = sse_reply({});
    auto step = std::make_shared<int>(0);
    const JsonValue id = message.id;
    reply.stream = [&, step, id]() -> std::optional<std::string> {
      switch ((*step)++) {
        case 0:
          return event(R"({"jsonrpc":"2.0","id":"s1","method":"ping"})");
        case 1:
          return event(R"({"jsonrpc":"2.0","id":"s2","method":"elicitation/create","params":{
            "mode":"form","message":"Your name?","requestedSchema":{"type":"object",
            "properties":{"name":{"type":"string"}}}}})");
        case 2: {
          // Hold the response until both answers have come back.
          std::unique_lock<std::mutex> lock(mutex);
          answered_cv.wait_for(lock, 3s, [&] { return answers.size() >= 2; });
          return event(make_result(id, text_result("thanks", false)));
        }
        default:
          return std::nullopt;
      }
    };
    return reply;
  });

  ClientOptions options = client_options(server.url());
  std::atomic<int> asked{0};
  options.elicit = [&](const ElicitationRequest& request) {
    ++asked;
    EXPECT_EQ(request.message, "Your name?");
    ElicitationResult result;
    result.action = "accept";
    result.content = json(R"({"name":"Ada"})");
    return result;
  };
  Client client(options);
  ASSERT_TRUE(client.connect().ok);
  const CallOutcome outcome = client.call_tool("echo", json(R"({"message":"x"})"), CallContext{});
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_EQ(text_of(outcome.result), "thanks");
  EXPECT_EQ(asked.load(), 1);

  std::lock_guard<std::mutex> lock(mutex);
  ASSERT_EQ(answers.size(), 2u);
  for (const RpcMessage& answer : answers) {
    if (answer.id.as_string() == "s1") EXPECT_TRUE(answer.result.is_object());
    if (answer.id.as_string() == "s2") {
      EXPECT_EQ(answer.result.get("action").as_string(), "accept");
      EXPECT_EQ(answer.result.get("content").get("name").as_string(), "Ada");
    }
  }
  for (const HttpRequest& request : server.http.requests()) {
    const RpcMessage message = parse_message(request.body);
    if (message.kind == MessageKind::Result) {
      EXPECT_EQ(request.header("Mcp-Session-Id"), "session-1");
    }
  }
}

TEST(McpHttpTest, AnOlderServerIsToldWhenARequestIsCancelled) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateful);
  server.set_hook([](const HttpRequest& request, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "tools/call" or request.header("Mcp-Session-Id").empty()) {
      return std::nullopt;
    }
    return endless_stream();
  });
  Client client(client_options(server.url()));
  ASSERT_TRUE(client.connect().ok);

  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(200ms);
    cancel.store(true);
  });
  const CallOutcome outcome =
      client.call_tool("echo", json(R"({"message":"x"})"), CallContext{}, {}, &cancel);
  canceller.join();
  EXPECT_FALSE(outcome.ok);

  const std::vector<HttpRequest> cancels = server.requests("notifications/cancelled");
  ASSERT_EQ(cancels.size(), 1u);
  const RpcMessage notice = parse_message(cancels[0].body);
  const RpcMessage call = parse_message(server.requests("tools/call").front().body);
  EXPECT_EQ(notice.params.get("requestId").dump(), call.id.dump());
  EXPECT_EQ(cancels[0].header("Mcp-Session-Id"), "session-1");
}

// 2025-11-25's polling: a server may close a stream it primed with an event id
// and expect the client back, after `retry`, with a GET and Last-Event-ID.
TEST(McpHttpTest, AStreamClosedEarlyIsResumedFromItsLastEventId) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateful);
  server.set_hook([](const HttpRequest& request, const RpcMessage& message) -> std::optional<HttpReply> {
    if (request.method == "GET") {
      if (request.header("Last-Event-ID") != "ev-1" or
          request.header("Mcp-Session-Id") != "session-1") {
        return HttpReply{400};
      }
      return sse_reply({event(R"({"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"resumed"}]}})", "ev-2")});
    }
    if (message.method != "tools/call" or request.header("Mcp-Session-Id").empty()) {
      return std::nullopt;
    }
    // Prime, say when to come back, and hang up.
    return sse_reply({"id: ev-1\ndata:\n\n", "retry: 50\n\n"});
  });

  HttpConfig config;
  config.url = server.url();
  HttpTransport transport(config, TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  // Shake hands by hand: initialize, then the negotiated version.
  RequestSpec initialize;
  initialize.id = JsonValue(1);
  initialize.method = "initialize";
  initialize.params = json(R"({"protocolVersion":"2025-11-25","capabilities":{},
    "clientInfo":{"name":"m8","version":"0"}})");
  initialize.deadline = Clock::now() + 5s;
  ASSERT_TRUE(transport.request(initialize).ok);
  transport.set_protocol(Era::Legacy, "2025-11-25");
  EXPECT_EQ(transport.session_id(), "session-1");

  const Reply reply = transport.request(call_spec(3, "x"));
  ASSERT_TRUE(reply.ok) << reply.error;
  EXPECT_EQ(text_of(reply.message.result), "resumed");
  const std::vector<HttpRequest> gets = server.requests("GET");
  ASSERT_EQ(gets.size(), 1u);
  EXPECT_EQ(gets[0].header("Accept"), "text/event-stream");
  EXPECT_EQ(gets[0].header("MCP-Protocol-Version"), "2025-11-25");
}

// ─────────────────────────── listening for changes ──────────────────────────

// 2026-07-28: subscriptions/listen is a POST whose SSE stream stays open and
// carries the list changes asked for; closing the client closes it.
TEST(McpHttpTest, AModernServerIsListenedToOnASubscriptionStream) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  std::atomic<bool> subscribed{false};
  server.set_hook([&](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method == "server/discover") {
      return json_reply(200, make_result(message.id, json(R"({"resultType":"complete",
        "supportedVersions":["2026-07-28"],"capabilities":{"tools":{"listChanged":true}},
        "_meta":{"io.modelcontextprotocol/serverInfo":{"name":"x","version":"1"}}})")));
    }
    if (message.method != "subscriptions/listen") return std::nullopt;
    EXPECT_TRUE(message.params.get("notifications").get("toolsListChanged").as_bool());
    subscribed.store(true);
    const std::string id = message.id.dump();
    HttpReply reply = sse_reply({});
    auto step = std::make_shared<int>(0);
    reply.stream = [step, id]() -> std::optional<std::string> {
      const int n = (*step)++;
      if (n == 0) {
        return event(R"({"jsonrpc":"2.0","method":"notifications/subscriptions/acknowledged","params":{"_meta":{"io.modelcontextprotocol/subscriptionId":)" +
                     id + R"(},"notifications":{"toolsListChanged":true}}})");
      }
      if (n == 1) {
        return event(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed","params":{"_meta":{"io.modelcontextprotocol/subscriptionId":)" +
                     id + "}}}");
      }
      if (n > 200) return std::nullopt;
      std::this_thread::sleep_for(50ms);
      return std::string(":\n\n");
    };
    return reply;
  });

  ClientOptions options = client_options(server.url());
  std::atomic<int> changed{0};
  options.on_list_changed = [&] { ++changed; };
  Client client(options);
  ASSERT_TRUE(client.connect().ok);
  for (int i = 0; i < 100 and changed.load() == 0; ++i) std::this_thread::sleep_for(20ms);
  EXPECT_TRUE(subscribed.load());
  EXPECT_GE(changed.load(), 1);

  const auto started = Clock::now();
  client.close();
  EXPECT_LT(Clock::now() - started, 1s);
  for (int i = 0; i < 40 and server.http.aborted_streams() == 0; ++i) std::this_thread::sleep_for(50ms);
  EXPECT_GE(server.http.aborted_streams(), 1);
}

// Before 2026-07-28: the GET stream, carrying the session.
TEST(McpHttpTest, AnOlderServerIsListenedToOnItsGetStream) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateful);
  std::atomic<int> gets{0};
  server.set_hook([&](const HttpRequest& request, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method == "initialize") {
      HttpReply reply = json_reply(200, make_result(message.id, json(R"({"protocolVersion":"2025-11-25",
        "capabilities":{"tools":{"listChanged":true}},"serverInfo":{"name":"x","version":"1"}})")));
      reply.headers.emplace_back("Mcp-Session-Id", "session-1");
      return reply;
    }
    if (request.method != "GET") return std::nullopt;
    ++gets;
    EXPECT_EQ(request.header("Mcp-Session-Id"), "session-1");
    EXPECT_EQ(request.header("Accept"), "text/event-stream");
    HttpReply reply = sse_reply({});
    auto step = std::make_shared<int>(0);
    reply.stream = [step]() -> std::optional<std::string> {
      const int n = (*step)++;
      if (n == 0) return event(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})");
      if (n > 200) return std::nullopt;
      std::this_thread::sleep_for(50ms);
      return std::string(":\n\n");
    };
    return reply;
  });

  ClientOptions options = client_options(server.url());
  std::atomic<int> changed{0};
  options.on_list_changed = [&] { ++changed; };
  Client client(options);
  ASSERT_TRUE(client.connect().ok);
  for (int i = 0; i < 100 and changed.load() == 0; ++i) std::this_thread::sleep_for(20ms);
  EXPECT_GE(gets.load(), 1);
  EXPECT_GE(changed.load(), 1);
  client.close();
}

// A server that does not stream (405) is not asked again on the same connection.
TEST(McpHttpTest, AServerWithoutAStreamIsNotPestered) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateful);
  server.set_hook([&](const HttpRequest&, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "initialize") return std::nullopt;
    HttpReply reply = json_reply(200, make_result(message.id, json(R"({"protocolVersion":"2025-11-25",
      "capabilities":{"tools":{"listChanged":true}},"serverInfo":{"name":"x","version":"1"}})")));
    reply.headers.emplace_back("Mcp-Session-Id", "session-1");
    return reply;
  });
  ClientOptions options = client_options(server.url());
  options.on_list_changed = [] {};
  Client client(options);
  ASSERT_TRUE(client.connect().ok);
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(server.requests("GET").size(), 1u);
  client.close();
}

// ─────────────────────────────── registry ───────────────────────────────────

struct McpHttpRegistryTest : ::testing::Test {
  std::filesystem::path dir;

  void SetUp() override {
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir = std::filesystem::temp_directory_path() / ("m8-mcp-http-" + std::string(info->name()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  void TearDown() override { std::filesystem::remove_all(dir); }

  RegistryOptions options() const {
    RegistryOptions o;
    o.workspace = dir.string();
    o.state_path = (dir / "state.json").string();
    o.trust_path = (dir / "trust.json").string();
    o.startup_timeout = 5s;
    o.call_timeout = 5s;
    return o;
  }

  static ServerConfig http_server(const std::string& name, const std::string& url) {
    std::vector<std::string> warnings;
    JsonValue root = JsonValue::object();
    JsonValue entry = JsonValue::object();
    entry.set("type", "http");
    entry.set("url", url);
    root["mcpServers"].set(name, entry);
    return parse_config(root.dump(), Scope::User, "test", warnings).front();
  }
};

// x-mcp-header parameters become Mcp-Param-* headers, and when the server says
// they no longer match the tool, the registry lists the tools again and
// retries once with what the new schema asks for.
TEST_F(McpHttpRegistryTest, MirrorsHeaderParametersAndRetriesOnAMismatch) {
  FakeHttpServer server(FakeHttpServer::Kind::Modern);
  server.set_tools(json(R"([{"name":"query","description":"Run a query",
    "inputSchema":{"type":"object","properties":{
      "region":{"type":"string","x-mcp-header":"Region"},
      "db":{"type":"string"},"sql":{"type":"string"}}}}])"));
  std::atomic<bool> want_db_header{false};
  server.set_hook([&](const HttpRequest& request, const RpcMessage& message) -> std::optional<HttpReply> {
    if (message.method != "tools/call") return std::nullopt;
    if (want_db_header.load() and request.header("Mcp-Param-Db").empty()) {
      return json_reply(400, error_body(message.id, kHeaderMismatch,
                                        "Header mismatch: Mcp-Param-Db is required"));
    }
    return std::nullopt;
  });

  Registry registry(options());
  registry.start({http_server("web", server.url())});
  const auto catalog = registry.wait_until_settled(5s);
  ASSERT_EQ(catalog->server("web")->state, ServerState::Connected) << catalog->server("web")->error;

  const tools::ToolResult first = registry.call_tool(
      "mcp__web__query", R"({"region":"us-west1","db":"main","sql":"select 1"})", CallContext{});
  ASSERT_TRUE(first.ok) << first.error;
  EXPECT_EQ(server.requests("tools/call").back().header("Mcp-Param-Region"), "us-west1");

  // The server's tool now mirrors db too, and insists on it.
  server.set_tools(json(R"([{"name":"query","description":"Run a query",
    "inputSchema":{"type":"object","properties":{
      "region":{"type":"string","x-mcp-header":"Region"},
      "db":{"type":"string","x-mcp-header":"Db"},"sql":{"type":"string"}}}}])"));
  want_db_header.store(true);
  const size_t lists_before = server.requests("tools/list").size();

  const tools::ToolResult second = registry.call_tool(
      "mcp__web__query", R"({"region":"us-west1","db":"main","sql":"select 2"})", CallContext{});
  ASSERT_TRUE(second.ok) << second.error;
  EXPECT_EQ(server.requests("tools/list").size(), lists_before + 1);
  const std::vector<HttpRequest> calls = server.requests("tools/call");
  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(calls[1].header("Mcp-Param-Db"), "");
  EXPECT_EQ(calls[2].header("Mcp-Param-Db"), "main");
  registry.shutdown();
}

TEST_F(McpHttpRegistryTest, ConnectsAnHttpServerWithNoFactoryInstalled) {
  FakeHttpServer server(FakeHttpServer::Kind::Stateful);
  Registry registry(options());
  registry.start({http_server("old", server.url())});
  const auto catalog = registry.wait_until_settled(5s);
  const CatalogServer* entry = catalog->server("old");
  ASSERT_NE(entry, nullptr);
  ASSERT_EQ(entry->state, ServerState::Connected) << entry->error;
  EXPECT_EQ(entry->transport, "http");
  EXPECT_EQ(entry->protocol_version, "2025-11-25");
  const tools::ToolResult result =
      registry.call_tool("mcp__old__echo", R"({"message":"via registry"})", CallContext{});
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.output, "echo: via registry");
  registry.shutdown();
  EXPECT_EQ(server.requests("DELETE").size(), 1u);
}

}  // namespace
}  // namespace mcp
