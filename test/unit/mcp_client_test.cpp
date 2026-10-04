// The MCP client over stdio, against a real child process (fake_mcp_server)
// that can speak either era of the protocol and misbehave on request.
//
// The era-detection cases are the heart of it: a dual-era client has to talk
// to modern servers, legacy servers, servers that ignore its probe, servers
// that crash on it, and modern servers that are merely slow to start — and
// get each one right without being told.

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <core/mcp/client.h>
#include <core/mcp/stdio_transport.h>
#include <core/util/process.h>

namespace mcp {
namespace {

using namespace std::chrono_literals;

std::string temp_record() {
  static std::atomic<int> counter{0};
  return (std::filesystem::temp_directory_path() /
          ("m8-mcp-record-" + std::to_string(::getpid()) + "-" +
           std::to_string(counter++) + ".txt"))
      .string();
}

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::stringstream out;
  out << in.rdbuf();
  return out.str();
}

bool eventually(const std::function<bool()>& condition,
                std::chrono::milliseconds limit = 3000ms) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return condition();
}

StdioConfig fake_config(const std::vector<std::string>& args) {
  StdioConfig config;
  config.server = "fake";
  config.command = M8_FAKE_MCP_SERVER;
  config.args = args;
  config.env = util::child_environment(false, {});
  config.close_grace = 300ms;
  config.term_grace = 300ms;
  return config;
}

ClientOptions fake_options(const std::vector<std::string>& args) {
  ClientOptions options;
  options.server = "fake";
  options.make_transport = [args](TransportHandlers handlers) {
    return std::make_unique<StdioTransport>(fake_config(args), std::move(handlers));
  };
  options.probe_timeout = 300ms;
  options.startup_timeout = 10s;
  options.list_timeout = 5s;
  options.call_timeout = 5s;
  options.read_timeout = 5s;
  return options;
}

std::string text_of(const CallOutcome& outcome) {
  const util::JsonValue& content = outcome.result.get("content");
  if (content.items().empty()) return std::string();
  return content.items()[0].get("text").as_string();
}

CallOutcome call(Client& client, const std::string& name,
                 const std::string& arguments = "{}") {
  return client.call_tool(name, *util::JsonValue::parse(arguments), CallContext{});
}

// ─────────────────────────────── era detection ──────────────────────────────

TEST(McpClientEraTest, ModernServerIsSpokenToStatelessly) {
  const std::string record = temp_record();
  Client client(fake_options({"--era=modern", "--record=" + record,
                              "--instructions=Use echo for echoing."}));
  const ConnectResult connected = client.connect();
  ASSERT_TRUE(connected.ok) << connected.error;
  EXPECT_EQ(Era::Modern, client.era());
  EXPECT_EQ(kModernVersion, client.protocol_version());
  EXPECT_EQ("fake-mcp-server", client.info().name);
  EXPECT_EQ("Use echo for echoing.", client.instructions());
  EXPECT_TRUE(client.capabilities().tools);

  const CallOutcome echoed = call(client, "echo", R"({"message":"hi"})");
  ASSERT_TRUE(echoed.ok) << echoed.error;
  EXPECT_EQ("hi", text_of(echoed));

  // Every request carried the per-request metadata; nothing was initialized.
  const std::string sent = slurp(record);
  EXPECT_NE(std::string::npos, sent.find("io.modelcontextprotocol/protocolVersion"));
  EXPECT_EQ(std::string::npos, sent.find("\"initialize\""));
  std::filesystem::remove(record);
}

TEST(McpClientEraTest, LegacyServerFallsBackToInitialize) {
  const std::string record = temp_record();
  Client client(fake_options(
      {"--era=legacy", "--legacy-version=2025-06-18", "--record=" + record}));
  const ConnectResult connected = client.connect();
  ASSERT_TRUE(connected.ok) << connected.error;
  EXPECT_EQ(Era::Legacy, client.era());
  EXPECT_EQ("2025-06-18", client.protocol_version());
  EXPECT_TRUE(call(client, "echo", R"({"message":"x"})").ok);
  EXPECT_NE(std::string::npos, slurp(record).find("notifications/initialized"));
  std::filesystem::remove(record);
}

TEST(McpClientEraTest, DualEraServerIsSpokenToInTheModernEra) {
  Client client(fake_options({"--era=dual"}));
  ASSERT_TRUE(client.connect().ok);
  EXPECT_EQ(Era::Modern, client.era());
}

TEST(McpClientEraTest, ASilentLegacyServerIsCaughtByTheProbeTimeout) {
  Client client(fake_options({"--era=silent-legacy"}));
  const ConnectResult connected = client.connect();
  ASSERT_TRUE(connected.ok) << connected.error;
  EXPECT_EQ(Era::Legacy, client.era());
  EXPECT_TRUE(call(client, "echo", R"({"message":"x"})").ok);
}

// npx downloading a package can take far longer than the probe wait. The
// discover answer that arrives late must still win.
TEST(McpClientEraTest, ASlowStartingModernServerIsNotMistakenForLegacy) {
  Client client(fake_options({"--era=modern", "--slow-start-ms=900"}));
  const ConnectResult connected = client.connect();
  ASSERT_TRUE(connected.ok) << connected.error;
  EXPECT_EQ(Era::Modern, client.era());
}

TEST(McpClientEraTest, AServerThatCrashesOnTheProbeIsRestartedIntoLegacy) {
  Client client(fake_options({"--era=crash-on-probe"}));
  const ConnectResult connected = client.connect();
  ASSERT_TRUE(connected.ok) << connected.error;
  EXPECT_EQ(Era::Legacy, client.era());
  EXPECT_TRUE(call(client, "echo", R"({"message":"x"})").ok);
}

TEST(McpClientEraTest, ARememberedEraSkipsTheProbe) {
  const std::string record = temp_record();
  ClientOptions options = fake_options({"--era=legacy", "--record=" + record});
  options.remembered_era = Era::Legacy;
  Client client(std::move(options));
  ASSERT_TRUE(client.connect().ok);
  EXPECT_EQ(std::string::npos, slurp(record).find("server/discover"));
  std::filesystem::remove(record);
}

TEST(McpClientEraTest, ForcingLegacyOnAModernOnlyServerFailsClearly) {
  ClientOptions options = fake_options({"--era=modern-only"});
  options.protocol = "legacy";
  Client client(std::move(options));
  const ConnectResult connected = client.connect();
  EXPECT_FALSE(connected.ok);
  EXPECT_NE(std::string::npos, connected.error.find("2026-07-28")) << connected.error;
}

// ───────────────────────────────── calls ────────────────────────────────────

TEST(McpClientTest, ListsEveryPage) {
  Client client(fake_options({"--era=modern", "--tools=120", "--page-size=25"}));
  ASSERT_TRUE(client.connect().ok);
  std::vector<ToolInfo> tools;
  std::string error;
  ASSERT_TRUE(client.list_tools(tools, error)) << error;
  EXPECT_EQ(128u, tools.size());
  EXPECT_EQ(300000, client.tools_ttl_ms());
}

TEST(McpClientTest, AToolThatFailsIsAResultNotATransportError) {
  Client client(fake_options({"--era=modern"}));
  ASSERT_TRUE(client.connect().ok);
  const CallOutcome failed = call(client, "fail");
  ASSERT_TRUE(failed.ok) << failed.error;
  EXPECT_TRUE(failed.result.get("isError").as_bool());

  const CallOutcome unknown = call(client, "no_such_tool");
  EXPECT_FALSE(unknown.ok);
  EXPECT_EQ(kInvalidParams, unknown.rpc_code);
}

TEST(McpClientTest, MultiRoundTripEchoesStateUnderANewId) {
  const std::string record = temp_record();
  Client client(fake_options({"--era=modern", "--record=" + record}));
  ASSERT_TRUE(client.connect().ok);
  const CallOutcome outcome = call(client, "stateful");
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_EQ("state ok", text_of(outcome));

  // Two tools/call requests, the second carrying the state, with distinct ids.
  std::vector<std::string> calls;
  std::istringstream lines(slurp(record));
  for (std::string line; std::getline(lines, line);) {
    if (line.find("tools/call") != std::string::npos) calls.push_back(line);
  }
  ASSERT_EQ(2u, calls.size());
  EXPECT_NE(std::string::npos, calls[1].find("\"requestState\":\"s1\""));
  const util::JsonValue first = *util::JsonValue::parse(calls[0]);
  const util::JsonValue second = *util::JsonValue::parse(calls[1]);
  EXPECT_NE(first.get("id"), second.get("id"));
  std::filesystem::remove(record);
}

TEST(McpClientTest, ConcurrentCallsAreMatchedToTheirReplies) {
  Client client(fake_options({"--era=modern"}));
  ASSERT_TRUE(client.connect().ok);
  std::vector<std::thread> threads;
  std::atomic<int> right{0};
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&client, &right, i] {
      const std::string message = "message-" + std::to_string(i);
      const CallOutcome outcome =
          call(client, "echo", "{\"message\":\"" + message + "\"}");
      if (outcome.ok and text_of(outcome) == message) ++right;
    });
  }
  for (std::thread& thread : threads) thread.join();
  EXPECT_EQ(8, right.load());
}

TEST(McpClientTest, ResourcesAndPrompts) {
  Client client(fake_options({"--era=legacy"}));
  ASSERT_TRUE(client.connect().ok);
  std::string error;

  std::vector<ResourceInfo> resources;
  ASSERT_TRUE(client.list_resources(resources, error)) << error;
  ASSERT_EQ(1u, resources.size());
  EXPECT_EQ("mem://greeting", resources[0].uri);

  std::vector<ResourceTemplateInfo> templates;
  ASSERT_TRUE(client.list_resource_templates(templates, error)) << error;
  ASSERT_EQ(1u, templates.size());

  const CallOutcome read = client.read_resource("mem://items/7", CallContext{});
  ASSERT_TRUE(read.ok) << read.error;
  EXPECT_EQ("item 7", read.result.get("contents").items()[0].get("text").as_string());

  const CallOutcome missing = client.read_resource("mem://nope", CallContext{});
  EXPECT_FALSE(missing.ok);
  EXPECT_NE(std::string::npos, missing.error.find("no resource")) << missing.error;

  std::vector<PromptInfo> prompts;
  ASSERT_TRUE(client.list_prompts(prompts, error)) << error;
  ASSERT_EQ(1u, prompts.size());
  EXPECT_TRUE(prompts[0].arguments[0].required);
  const CallOutcome prompt = client.get_prompt(
      "review", *util::JsonValue::parse(R"({"code":"int x;"})"), CallContext{});
  ASSERT_TRUE(prompt.ok) << prompt.error;
}

// ─────────────────────────────── elicitation ────────────────────────────────

ElicitationHandler answering(const std::string& name, std::vector<ElicitationRequest>* seen) {
  return [name, seen](const ElicitationRequest& request) {
    if (seen != nullptr) seen->push_back(request);
    ElicitationResult result;
    result.action = "accept";
    result.content = util::JsonValue::object();
    result.content.set("name", name);
    return result;
  };
}

TEST(McpElicitationTest, ModernServersAskThroughInputRequiredResults) {
  std::vector<ElicitationRequest> seen;
  ClientOptions options = fake_options({"--era=modern"});
  options.elicit = answering("Ada", &seen);
  Client client(std::move(options));
  ASSERT_TRUE(client.connect().ok);

  CallContext context;
  context.agent_label = "root";
  const CallOutcome outcome =
      client.call_tool("ask_name", util::JsonValue::object(), context);
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_EQ("hello Ada (state asked-once)", text_of(outcome));
  ASSERT_EQ(1u, seen.size());
  EXPECT_EQ("fake", seen[0].server);
  EXPECT_EQ("root", seen[0].agent_label);
  EXPECT_EQ("form", seen[0].mode);
  EXPECT_EQ("What is your name?", seen[0].message);
}

TEST(McpElicitationTest, LegacyServersAskWithARequestOfTheirOwn) {
  std::vector<ElicitationRequest> seen;
  ClientOptions options = fake_options({"--era=legacy"});
  options.elicit = answering("Grace", &seen);
  Client client(std::move(options));
  ASSERT_TRUE(client.connect().ok);
  const CallOutcome outcome = call(client, "ask_name");
  ASSERT_TRUE(outcome.ok) << outcome.error;
  EXPECT_EQ("hello Grace", text_of(outcome));
  EXPECT_EQ(1u, seen.size());
}

TEST(McpElicitationTest, UrlModeAndDeclining) {
  ClientOptions options = fake_options({"--era=modern"});
  options.elicit = [](const ElicitationRequest& request) {
    ElicitationResult result;
    result.action = request.mode == "url" ? "accept" : "decline";
    return result;
  };
  Client client(std::move(options));
  ASSERT_TRUE(client.connect().ok);
  EXPECT_EQ("visited", text_of(call(client, "ask_url")));
  EXPECT_EQ("declined", text_of(call(client, "ask_name")));
}

TEST(McpElicitationTest, WithoutAHandlerNoCapabilityIsDeclared) {
  const std::string record = temp_record();
  Client client(fake_options({"--era=modern", "--record=" + record}));
  ASSERT_TRUE(client.connect().ok);
  EXPECT_EQ(std::string::npos, slurp(record).find("elicitation"));
  // A server that asks anyway is declined rather than left hanging.
  EXPECT_EQ("declined", text_of(call(client, "ask_name")));
  std::filesystem::remove(record);
}

// ───────────────────────────── server behaviour ─────────────────────────────

TEST(McpClientTest, ALegacyServersPingIsAnswered) {
  const std::string record = temp_record();
  Client client(fake_options({"--era=legacy", "--ping-client", "--record=" + record}));
  ASSERT_TRUE(client.connect().ok);
  EXPECT_TRUE(eventually([&] {
    return slurp(record).find("PING-ANSWERED") != std::string::npos;
  }));
  std::filesystem::remove(record);
}

TEST(McpClientTest, ATimeoutIsCancelledOnTheWire) {
  const std::string record = temp_record();
  ClientOptions options = fake_options({"--era=modern", "--record=" + record});
  options.call_timeout = 300ms;
  Client client(std::move(options));
  ASSERT_TRUE(client.connect().ok);
  const CallOutcome outcome = call(client, "sleep", R"({"ms":5000})");
  EXPECT_FALSE(outcome.ok);
  EXPECT_TRUE(outcome.timed_out);
  EXPECT_TRUE(eventually([&] {
    return slurp(record).find("notifications/cancelled") != std::string::npos;
  }));
  // The connection survives a timeout.
  EXPECT_TRUE(call(client, "echo", R"({"message":"still here"})").ok);
  std::filesystem::remove(record);
}

TEST(McpClientTest, ListChangedReachesTheClient) {
  std::atomic<int> changed{0};
  ClientOptions options = fake_options({"--era=legacy", "--list-changed"});
  options.on_list_changed = [&changed] { ++changed; };
  Client client(std::move(options));
  ASSERT_TRUE(client.connect().ok);
  EXPECT_TRUE(call(client, "echo", R"({"message":"x"})").ok);
  EXPECT_TRUE(eventually([&] { return changed.load() > 0; }));
}

// 2026-07-28: list changes come on a subscription the client holds open — one
// request, acknowledged, and exempt from the timeouts every other request has.
TEST(McpClientTest, AModernServerIsListenedToOnASubscription) {
  const std::string record = (std::filesystem::temp_directory_path() /
                              "m8-mcp-client-subscription.record").string();
  std::filesystem::remove(record);
  std::atomic<int> changed{0};
  ClientOptions options = fake_options({"--era=modern", "--list-changed", "--record=" + record});
  options.call_timeout = 300ms;
  options.list_timeout = 300ms;
  options.on_list_changed = [&changed] { ++changed; };
  Client client(std::move(options));
  ASSERT_TRUE(client.connect().ok);
  ASSERT_TRUE(eventually([&] { return slurp(record).find("SUBSCRIBED") != std::string::npos; }));
  EXPECT_NE(slurp(record).find(R"("toolsListChanged":true)"), std::string::npos);

  // Longer than any request may take: still open.
  std::this_thread::sleep_for(700ms);
  EXPECT_EQ(slurp(record).find("SUBSCRIPTION-CANCELLED"), std::string::npos);

  EXPECT_TRUE(call(client, "echo", R"({"message":"x"})").ok);
  EXPECT_TRUE(eventually([&] { return changed.load() > 0; }));
  client.close();
  std::filesystem::remove(record);
}

// Without a listener (nobody set on_list_changed) nothing is held open.
TEST(McpClientTest, NoOneListeningMeansNoSubscription) {
  const std::string record = (std::filesystem::temp_directory_path() /
                              "m8-mcp-client-no-subscription.record").string();
  std::filesystem::remove(record);
  Client client(fake_options({"--era=modern", "--record=" + record}));
  ASSERT_TRUE(client.connect().ok);
  EXPECT_TRUE(call(client, "echo", R"({"message":"x"})").ok);
  EXPECT_EQ(slurp(record).find("subscriptions/listen"), std::string::npos);
  client.close();
  std::filesystem::remove(record);
}

// A server restarted after a crash gets the subscription again: the old one
// died with the old process.
TEST(McpClientTest, TheSubscriptionIsRenewedAfterARestart) {
  const std::string record = (std::filesystem::temp_directory_path() /
                              "m8-mcp-client-resubscribe.record").string();
  std::filesystem::remove(record);
  // discover (1), subscriptions/listen (2), echo (3); the 4th request crashes it.
  ClientOptions options = fake_options({"--era=modern", "--crash-after=3", "--record=" + record});
  options.on_list_changed = [] {};
  Client client(std::move(options));
  ASSERT_TRUE(client.connect().ok);
  ASSERT_TRUE(eventually([&] { return slurp(record).find("SUBSCRIBED") != std::string::npos; }));
  EXPECT_TRUE(call(client, "echo", R"({"message":"one"})").ok);
  EXPECT_FALSE(call(client, "echo", R"({"message":"crash"})").ok);
  EXPECT_TRUE(call(client, "echo", R"({"message":"restarted"})").ok);
  EXPECT_TRUE(eventually([&] {
    const std::string text = slurp(record);
    const size_t first = text.find("SUBSCRIBED");
    return first != std::string::npos and text.find("SUBSCRIBED", first + 1) != std::string::npos;
  }));
  client.close();
  std::filesystem::remove(record);
}

TEST(McpClientTest, BannerNoiseAndStderrAreTolerated) {
  Client client(fake_options({"--era=modern", "--banner", "--stderr-spam"}));
  ASSERT_TRUE(client.connect().ok);
  EXPECT_TRUE(eventually([&] {
    return client.stderr_tail().find("spam line 199") != std::string::npos;
  }));
}

TEST(McpClientTest, ACrashedServerIsRestartedOnTheNextCall) {
  // Requests: discover (1), echo (2) answered; the third request crashes it.
  Client client(fake_options({"--era=modern", "--crash-after=2"}));
  ASSERT_TRUE(client.connect().ok);
  EXPECT_TRUE(call(client, "echo", R"({"message":"one"})").ok);
  const CallOutcome crashed = call(client, "echo", R"({"message":"two"})");
  EXPECT_FALSE(crashed.ok);
  EXPECT_NE(std::string::npos, crashed.error.find("exited with status 3")) << crashed.error;
  EXPECT_NE(std::string::npos, crashed.error.find("crashing on purpose")) << crashed.error;

  const CallOutcome restarted = call(client, "echo", R"({"message":"three"})");
  ASSERT_TRUE(restarted.ok) << restarted.error;
  EXPECT_EQ("three", text_of(restarted));
}

TEST(McpClientTest, AServerThatKeepsCrashingIsLeftDown) {
  ClientOptions options = fake_options({"--era=modern", "--crash-after=1"});
  options.max_restarts = 2;
  Client client(std::move(options));
  ASSERT_TRUE(client.connect().ok);
  std::string last;
  for (int i = 0; i < 4; ++i) last = call(client, "echo", R"({"message":"x"})").error;
  EXPECT_NE(std::string::npos, last.find("left down")) << last;
}

// ───────────────────────────── the transport ────────────────────────────────

bool process_gone(pid_t pid) { return ::kill(pid, 0) != 0 and errno == ESRCH; }

TEST(McpStdioTransportTest, ShutdownEndsAServerThatIgnoresEofAndSigterm) {
  StdioTransport transport(fake_config({"--era=modern", "--ignore-eof", "--ignore-term"}),
                           TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  const pid_t pid = transport.pid();
  ASSERT_GT(pid, 0);

  const auto started = std::chrono::steady_clock::now();
  transport.close();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
  EXPECT_TRUE(process_gone(pid));
  EXPECT_FALSE(transport.alive());
}

TEST(McpStdioTransportTest, APoliteServerExitsOnEof) {
  StdioTransport transport(fake_config({"--era=modern"}), TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  const pid_t pid = transport.pid();
  const auto started = std::chrono::steady_clock::now();
  transport.close();
  // Well inside the close grace: no signal was needed.
  EXPECT_LT(std::chrono::steady_clock::now() - started, 290ms);
  EXPECT_TRUE(process_gone(pid));
}

TEST(McpStdioTransportTest, RequestsAfterExitFailFast) {
  StdioTransport transport(fake_config({"--era=modern", "--crash-after=0"}),
                           TransportHandlers{});
  std::string error;
  ASSERT_TRUE(transport.start(error)) << error;
  RequestSpec spec;
  spec.id = util::JsonValue(1);
  spec.method = "server/discover";
  spec.deadline = Clock::now() + 5s;
  const Reply first = transport.request(spec);
  EXPECT_FALSE(first.ok);
  EXPECT_TRUE(first.exited);

  spec.id = util::JsonValue(2);
  const auto started = std::chrono::steady_clock::now();
  const Reply second = transport.request(spec);
  EXPECT_FALSE(second.ok);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
}

TEST(McpProcessTest, ChildEnvironmentIsAnAllowlistPlusOverrides) {
  ::setenv("M8_TEST_SECRET_TOKEN", "do-not-leak", 1);
  const util::EnvList env = util::child_environment(false, {{"EXTRA", "1"}});
  EXPECT_EQ("", util::env_lookup(env, "M8_TEST_SECRET_TOKEN"));
  EXPECT_EQ("1", util::env_lookup(env, "EXTRA"));
  EXPECT_FALSE(util::env_lookup(env, "PATH").empty());
  const util::EnvList everything = util::child_environment(true, {});
  EXPECT_EQ("do-not-leak", util::env_lookup(everything, "M8_TEST_SECRET_TOKEN"));
  ::unsetenv("M8_TEST_SECRET_TOKEN");
}

TEST(McpProcessTest, FindsExecutablesOnTheGivenPath) {
  EXPECT_EQ("/bin/sh", util::find_executable("sh", "/nonexistent:/bin"));
  EXPECT_EQ("", util::find_executable("definitely-not-a-command-m8", "/bin:/usr/bin"));
  EXPECT_EQ("/bin/sh", util::find_executable("/bin/sh", ""));
}

// A program m8 picks itself (the browser opener) comes only from PATH's
// absolute directories: an empty or relative entry is the current directory,
// where a checkout could put a `tool` of its own.
TEST(McpProcessTest, FindsInstalledExecutablesOnlyOnAbsolutePathEntries) {
  const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("m8-installed-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  for (const std::filesystem::path& dir : {root, root / "rel", root / "abs" / "bin"}) {
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "tool") << "#!/bin/sh\n";
    std::filesystem::permissions(dir / "tool", std::filesystem::perms::owner_all);
  }
  const std::filesystem::path saved = std::filesystem::current_path();
  std::filesystem::current_path(root);

  const std::string bin = (root / "abs" / "bin").string();
  EXPECT_EQ(bin + "/tool", util::find_installed_executable("tool", ":rel:" + bin));
  EXPECT_EQ("", util::find_installed_executable("tool", ":rel"));
  EXPECT_EQ("", util::find_installed_executable("tool", ""));
  EXPECT_EQ("", util::find_installed_executable("./tool", bin));
  // What it guards against: the plain lookup takes the empty entry as ".".
  EXPECT_EQ("./tool", util::find_executable("tool", ":rel"));

  std::filesystem::current_path(saved);
  std::filesystem::remove_all(root);
}

// posix_spawn would take a bare name as a file in the current directory, so a
// command nobody resolved is refused rather than run.
TEST(McpProcessTest, SpawnRefusesABareCommand) {
  util::SpawnOptions options;
  options.command = "sh";
  util::SpawnedProcess process;
  std::string error;
  EXPECT_FALSE(util::spawn_process(options, process, error));
  EXPECT_NE(error.find("not resolved to a path"), std::string::npos) << error;
  EXPECT_EQ(-1, process.pid);
}

}  // namespace
}  // namespace mcp
