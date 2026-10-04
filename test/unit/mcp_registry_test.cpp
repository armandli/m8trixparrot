// The registry: every configured server connected in the background behind one
// catalog snapshot, with approvals gating workspace servers, and calls routed
// by exposed tool name. Runs fake_mcp_server as real child processes.

#include <signal.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <core/mcp/registry.h>
#include <core/mcp/resource_tool.h>

namespace mcp {
namespace {

using namespace std::chrono_literals;

struct McpRegistryTest : ::testing::Test {
  std::filesystem::path dir;

  void SetUp() override {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    dir = std::filesystem::temp_directory_path() /
          ("m8-mcp-registry-" + std::string(info->name()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "ws");
  }
  void TearDown() override { std::filesystem::remove_all(dir); }

  RegistryOptions options() const {
    RegistryOptions o;
    o.workspace = (dir / "ws").string();
    o.logs_dir = (dir / "logs").string();
    o.state_path = (dir / "ws/.m8/mcp_state.json").string();
    o.trust_path = (dir / "trust.json").string();
    o.probe_timeout = 300ms;
    o.startup_timeout = 10s;
    o.call_timeout = 5s;
    o.close_grace = 300ms;
    o.term_grace = 300ms;
    return o;
  }

  std::string record(const std::string& name) const {
    return (dir / (name + ".record")).string();
  }

  ServerConfig server(const std::string& name, std::vector<std::string> args,
                      Scope scope = Scope::User) const {
    args.push_back("--record=" + record(name));
    std::vector<std::string> warnings;
    util::JsonValue entry = util::JsonValue::object();
    entry.set("command", M8_FAKE_MCP_SERVER);
    util::JsonValue list = util::JsonValue::array();
    for (const std::string& arg : args) list.push_back(arg);
    entry.set("args", list);
    util::JsonValue root = util::JsonValue::object();
    root["mcpServers"].set(name, entry);
    ServerConfig config =
        parse_config(root.dump(), scope, "test", warnings).front();
    return config;
  }

  static std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
  }

  static ServerState state_of(const Registry& registry, const std::string& name) {
    const CatalogServer* entry = registry.snapshot()->server(name);
    return entry == nullptr ? ServerState::Failed : entry->state;
  }
};

TEST_F(McpRegistryTest, ConnectsInTheBackgroundAndRoutesCalls) {
  Registry registry(options());
  std::vector<std::string> notices;
  std::mutex notices_mutex;
  registry.set_observer([&](const RegistryEvent& event) {
    std::lock_guard<std::mutex> lock(notices_mutex);
    notices.push_back(event.text);
  });
  registry.start({server("fake", {"--era=modern", "--instructions=be nice"})});

  const auto catalog = registry.wait_until_settled(10s);
  ASSERT_EQ(ServerState::Connected, catalog->server("fake")->state)
      << catalog->server("fake")->error;
  EXPECT_EQ("be nice", catalog->server("fake")->instructions);
  ASSERT_NE(nullptr, catalog->find("mcp__fake__echo"));
  EXPECT_EQ(8u, catalog->server("fake")->tool_count);

  const tools::ToolResult echoed =
      registry.call_tool("mcp__fake__echo", R"({"message":"hello"})", CallContext{});
  ASSERT_TRUE(echoed.ok) << echoed.error;
  EXPECT_EQ("hello", echoed.output);

  // Strings where the schema wants numbers are fixed before they go out.
  const tools::ToolResult added =
      registry.call_tool("mcp__fake__add", R"({"a":"2","b":3})", CallContext{});
  ASSERT_TRUE(added.ok) << added.error;
  EXPECT_EQ("5.000000", added.output);

  // isError comes back as a failed tool result, in the server's words.
  const tools::ToolResult failed = registry.call_tool("mcp__fake__fail", "{}", CallContext{});
  EXPECT_FALSE(failed.ok);
  EXPECT_EQ("it failed", failed.error);

  const tools::ToolResult unknown = registry.call_tool("mcp__fake__nope", "{}", CallContext{});
  EXPECT_FALSE(unknown.ok);
  EXPECT_NE(std::string::npos, unknown.error.find("tool_search"));

  std::lock_guard<std::mutex> lock(notices_mutex);
  bool announced = false;
  for (const std::string& notice : notices) {
    announced = announced or notice.find("fake connected (8 tools)") != std::string::npos;
  }
  EXPECT_TRUE(announced);
}

TEST_F(McpRegistryTest, WorkspaceServersWaitForApproval) {
  {
    Registry registry(options());
    registry.start({server("proj", {"--era=modern"}, Scope::Project)});
    registry.wait_until_settled(1s);
    EXPECT_EQ(ServerState::NeedsApproval, state_of(registry, "proj"));
    EXPECT_EQ((std::vector<std::string>{"proj"}), registry.pending_approval());
    // Never started: the fake server records its pid the moment it runs.
    EXPECT_FALSE(std::filesystem::exists(record("proj")));

    std::string error;
    ASSERT_TRUE(registry.approve("proj", error)) << error;
    registry.wait_until_settled(10s);
    EXPECT_EQ(ServerState::Connected, state_of(registry, "proj"));
  }
  // The approval is remembered for the same configuration...
  {
    Registry registry(options());
    registry.start({server("proj", {"--era=modern"}, Scope::Project)});
    registry.wait_until_settled(10s);
    EXPECT_EQ(ServerState::Connected, state_of(registry, "proj"));
  }
  // ...and not for a changed one.
  {
    Registry registry(options());
    registry.start({server("proj", {"--era=modern", "--tools=1"}, Scope::Project)});
    registry.wait_until_settled(1s);
    EXPECT_EQ(ServerState::NeedsApproval, state_of(registry, "proj"));
  }
}

TEST_F(McpRegistryTest, DisablingIsRememberedPerWorkspace) {
  {
    Registry registry(options());
    registry.start({server("fake", {"--era=modern"})});
    registry.wait_until_settled(10s);
    std::string error;
    ASSERT_TRUE(registry.set_enabled("fake", false, error)) << error;
    EXPECT_EQ(ServerState::Disabled, state_of(registry, "fake"));
    EXPECT_EQ(nullptr, registry.snapshot()->find("mcp__fake__echo"));
  }
  Registry registry(options());
  registry.start({server("fake", {"--era=modern"})});
  registry.wait_until_settled(1s);
  EXPECT_EQ(ServerState::Disabled, state_of(registry, "fake"));
  std::string error;
  ASSERT_TRUE(registry.set_enabled("fake", true, error)) << error;
  registry.wait_until_settled(10s);
  EXPECT_EQ(ServerState::Connected, state_of(registry, "fake"));
}

TEST_F(McpRegistryTest, BrokenEntriesFailWithTheirReason) {
  ServerConfig missing_var = server("vars", {"--era=modern"});
  missing_var.env.emplace_back("TOKEN", "${M8_SURELY_UNSET_VARIABLE}");
  ServerConfig missing_command = server("nocmd", {});
  missing_command.command = "m8-no-such-command-anywhere";
  std::vector<std::string> warnings;
  ServerConfig problem = parse_config(R"({"mcpServers":{"old":{"type":"sse","url":"https://e"}}})",
                                      Scope::User, "test", warnings)
                             .front();

  Registry registry(options());
  registry.start({missing_var, missing_command, problem});
  const auto catalog = registry.wait_until_settled(5s);
  EXPECT_NE(std::string::npos,
            catalog->server("vars")->error.find("M8_SURELY_UNSET_VARIABLE"));
  EXPECT_NE(std::string::npos, catalog->server("nocmd")->error.find("not found"));
  EXPECT_EQ(ServerState::Failed, catalog->server("old")->state);
}

TEST_F(McpRegistryTest, ToolFiltersApply) {
  ServerConfig config = server("fake", {"--era=modern"});
  config.disabled_tools = {"fail"};
  Registry registry(options());
  registry.start({config});
  const auto catalog = registry.wait_until_settled(10s);
  EXPECT_EQ(nullptr, catalog->find("mcp__fake__fail"));
  EXPECT_NE(nullptr, catalog->find("mcp__fake__echo"));
  EXPECT_EQ(7u, catalog->server("fake")->tool_count);
}

TEST_F(McpRegistryTest, AListChangeRefreshesTheCatalog) {
  Registry registry(options());
  registry.start({server("fake", {"--era=legacy", "--list-changed"})});
  const auto before = registry.wait_until_settled(10s);
  ASSERT_EQ(ServerState::Connected, before->server("fake")->state);
  ASSERT_TRUE(registry.call_tool("mcp__fake__echo", R"({"message":"x"})", CallContext{}).ok);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (registry.snapshot()->generation == before->generation and
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_GT(registry.snapshot()->generation, before->generation);
}

TEST_F(McpRegistryTest, ResourcesAndPrompts) {
  Registry registry(options());
  registry.start({server("fake", {"--era=modern"})});
  ASSERT_EQ(ServerState::Connected, registry.wait_until_settled(10s)->server("fake")->state);
  EXPECT_TRUE(registry.snapshot()->any_resources());

  CallContext context;
  ResourceTool tool{registry, context};
  tools::ToolArgs list;
  list["action"] = std::string("list");
  const tools::ToolResult listed = tool.execute(list);
  ASSERT_TRUE(listed.ok) << listed.error;
  EXPECT_NE(std::string::npos, listed.output.find("mem://greeting"));

  tools::ToolArgs read;
  read["action"] = std::string("read");
  read["uri"] = std::string("mem://greeting");
  const tools::ToolResult contents = tool.execute(read);
  ASSERT_TRUE(contents.ok) << contents.error;
  EXPECT_NE(std::string::npos, contents.output.find("hello"));

  ASSERT_EQ(1u, registry.snapshot()->server("fake")->prompts.size());
  const CallOutcome prompt = registry.get_prompt(
      "fake", "review", *util::JsonValue::parse(R"({"code":"x"})"), context);
  ASSERT_TRUE(prompt.ok) << prompt.error;
}

TEST_F(McpRegistryTest, ShutdownStopsEveryServerQuickly) {
  Registry registry(options());
  registry.start({server("one", {"--era=modern"}), server("two", {"--era=legacy"}),
                  server("stubborn", {"--era=modern", "--ignore-eof", "--ignore-term"})});
  registry.wait_until_settled(10s);

  std::vector<pid_t> pids;
  for (const char* name : {"one", "two", "stubborn"}) {
    std::istringstream lines(slurp(record(name)));
    std::string first;
    std::getline(lines, first);
    ASSERT_EQ(0u, first.rfind("PID ", 0)) << name;
    pids.push_back(static_cast<pid_t>(std::stol(first.substr(4))));
  }

  const auto started = std::chrono::steady_clock::now();
  registry.shutdown();
  // The ladders run in parallel: one stubborn server, not three in a row.
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2500ms);
  for (const pid_t pid : pids) EXPECT_TRUE(::kill(pid, 0) != 0) << pid;

  const tools::ToolResult after = registry.call_tool("mcp__one__echo", "{}", CallContext{});
  EXPECT_FALSE(after.ok);
}

TEST_F(McpRegistryTest, ShutdownDoesNotWaitOutASlowHandshake) {
  Registry registry(options());
  registry.start({server("slow", {"--era=modern", "--slow-start-ms=8000"})});
  std::this_thread::sleep_for(500ms);  // past the probe: both questions asked
  const auto started = std::chrono::steady_clock::now();
  registry.shutdown();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2500ms);
}

TEST_F(McpRegistryTest, ElicitationReachesTheHandler) {
  Registry registry(options());
  std::atomic<int> asked{0};
  registry.set_elicitation_handler([&](const ElicitationRequest& request) {
    ++asked;
    ElicitationResult result;
    result.action = "accept";
    result.content = util::JsonValue::object();
    result.content.set("name", request.server);
    return result;
  });
  registry.start({server("fake", {"--era=modern"})});
  registry.wait_until_settled(10s);
  const tools::ToolResult answered =
      registry.call_tool("mcp__fake__ask_name", "{}", CallContext{});
  ASSERT_TRUE(answered.ok) << answered.error;
  EXPECT_NE(std::string::npos, answered.output.find("hello fake"));
  EXPECT_EQ(1, asked.load());
}

}  // namespace
}  // namespace mcp
