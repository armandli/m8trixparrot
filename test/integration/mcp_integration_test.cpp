// Live MCP tests against the official reference server,
// @modelcontextprotocol/server-everything, fetched with npx: over stdio and
// over Streamable HTTP, through the same registry m8 uses. With an Ollama
// model available, also: every schema the server's tools lower to is accepted
// by /api/chat, and a real agent turn finds `echo` through tool_search and
// calls it.
//
// Each test self-skips when npx, the network (to fetch the package), Ollama or
// the model is missing (OLLAMA_HOST / M8_TEST_MODEL as for the other suites).
//
// Run with:  make integration-test   (or  ctest --test-dir build -L integration)

#include <signal.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent/agent.h>
#include <core/agent/agent_pool.h>
#include <core/mcp/registry.h>
#include <core/oc/basic_ollama_client.h>
#include <core/oc/ollama_client.h>
#include <core/policy/policy.h>
#include <core/util/process.h>

namespace mcp {
namespace {

using namespace std::chrono_literals;

constexpr const char* kPackage = "@modelcontextprotocol/server-everything";

std::string npx() {
  const char* path = std::getenv("PATH");
  return util::find_executable("npx", path != nullptr ? path : "", "");
}

std::string ollama_host() {
  const char* host = std::getenv("OLLAMA_HOST");
  return host != nullptr and *host != '\0' ? host : "http://localhost:11434";
}

std::string test_model() {
  const char* model = std::getenv("M8_TEST_MODEL");
  return model != nullptr and *model != '\0' ? model : "qwen3.8:27b-mlx";
}

struct McpIntegrationTest : ::testing::Test {
  std::filesystem::path dir;

  void SetUp() override {
    if (npx().empty()) GTEST_SKIP() << "npx is not on PATH";
    dir = std::filesystem::temp_directory_path() /
          ("m8-mcp-integration-" +
           std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  RegistryOptions options() const {
    RegistryOptions o;
    o.workspace = dir.string();
    o.logs_dir = (dir / "logs").string();
    // npx may download the package on first use.
    o.startup_timeout = 120s;
    o.call_timeout = 30s;
    return o;
  }

  static ServerConfig config(const std::string& entry) {
    std::vector<std::string> warnings;
    return parse_config(R"({"mcpServers":{"everything":)" + entry + "}}", Scope::User,
                        "integration", warnings)
        .front();
  }

  // Connected, or the test is skipped: no network for npx is not a failure.
  static std::shared_ptr<const Catalog> connected(Registry& registry) {
    const auto catalog = registry.wait_until_settled(150s);
    const CatalogServer* server = catalog->server("everything");
    if (server == nullptr or server->state != ServerState::Connected) {
      return nullptr;
    }
    return catalog;
  }

  // The checks both transports must pass.
  static void exercise(Registry& registry, const Catalog& catalog) {
    EXPECT_GT(catalog.tools.size(), 5u);
    const tools::ToolResult echoed =
        registry.call_tool("mcp__everything__echo", R"({"message":"integration-ping"})", CallContext{});
    ASSERT_TRUE(echoed.ok) << echoed.error;
    EXPECT_NE(echoed.output.find("integration-ping"), std::string::npos) << echoed.output;

    const tools::ToolResult listed = registry.resource("list", "everything", "", CallContext{});
    ASSERT_TRUE(listed.ok) << listed.error;
    const tools::ToolResult read = registry.resource(
        "read", "everything", "demo://resource/static/document/architecture.md", CallContext{});
    ASSERT_TRUE(read.ok) << read.error;
    EXPECT_FALSE(read.output.empty());

    const CatalogServer* server = catalog.server("everything");
    ASSERT_NE(server, nullptr);
    ASSERT_FALSE(server->prompts.empty());
    const PromptInfo* simple = nullptr;
    for (const PromptInfo& prompt : server->prompts) {
      if (prompt.arguments.empty()) simple = &prompt;
    }
    ASSERT_NE(simple, nullptr);
    const CallOutcome prompt =
        registry.get_prompt("everything", simple->name, util::JsonValue::object(), CallContext{});
    ASSERT_TRUE(prompt.ok) << prompt.error;
    EXPECT_GT(prompt.result.get("messages").size(), 0u);
  }
};

TEST_F(McpIntegrationTest, TheReferenceServerOverStdio) {
  Registry registry(options());
  registry.start({config(std::string(R"({"command":"npx","args":["-y",")") + kPackage +
                         R"(","stdio"]})")});
  const auto catalog = connected(registry);
  if (not catalog) GTEST_SKIP() << "could not start " << kPackage << " (no network for npx?)";
  exercise(registry, *catalog);
  registry.shutdown();
}

TEST_F(McpIntegrationTest, TheReferenceServerOverStreamableHttp) {
  // Its own process, on a port of ours.
  const int port = 39000 + static_cast<int>(::getpid() % 1000);
  util::SpawnOptions spawn;
  spawn.command = npx();
  spawn.args = {"-y", kPackage, "streamableHttp"};
  spawn.env = util::child_environment(true, {{"PORT", std::to_string(port)}});
  util::SpawnedProcess process;
  std::string error;
  ASSERT_TRUE(util::spawn_process(spawn, process, error)) << error;
  struct Stop {
    util::SpawnedProcess& process;
    ~Stop() {
      ::kill(-process.pid, SIGTERM);
      int status = 0;
      for (int i = 0; i < 50 and not util::reap(process.pid, status); ++i) {
        std::this_thread::sleep_for(100ms);
      }
      ::kill(-process.pid, SIGKILL);
      util::reap(process.pid, status);
    }
  } stop{process};

  RegistryOptions o = options();
  o.startup_timeout = 5s;
  // npx may still be fetching: try until it answers, for a while.
  std::shared_ptr<const Catalog> catalog;
  for (int attempt = 0; attempt < 24 and not catalog; ++attempt) {
    Registry probe(o);
    probe.start({config(R"({"type":"http","url":"http://127.0.0.1:)" + std::to_string(port) +
                        R"(/mcp"})")});
    if (connected(probe)) catalog = probe.snapshot();
    probe.shutdown();
    if (not catalog) std::this_thread::sleep_for(5s);
  }
  if (not catalog) GTEST_SKIP() << "could not start " << kPackage << " over HTTP";

  Registry registry(o);
  registry.start({config(R"({"type":"http","url":"http://127.0.0.1:)" + std::to_string(port) +
                         R"(/mcp"})")});
  catalog = connected(registry);
  ASSERT_NE(catalog, nullptr);
  exercise(registry, *catalog);
  registry.shutdown();
}

// Ollama unmarshals tool parameters into Go structs, and one schema it cannot
// read fails the whole request: every lowered schema must go through.
TEST_F(McpIntegrationTest, OllamaAcceptsEveryLoweredSchema) {
  const oc::BasicOllamaClient ollama(ollama_host());
  if (not ollama.show(test_model()).ok) {
    GTEST_SKIP() << "Ollama model '" << test_model() << "' not reachable at " << ollama_host();
  }
  Registry registry(options());
  registry.start({config(std::string(R"({"command":"npx","args":["-y",")") + kPackage +
                         R"(","stdio"]})")});
  const auto catalog = connected(registry);
  if (not catalog) GTEST_SKIP() << "could not start " << kPackage;

  std::vector<std::string> schemas;
  for (const CatalogTool& tool : catalog->tools) schemas.push_back(tool.schema_json);
  oc::ChatMessage message;
  message.role = "user";
  message.content = "Reply with the single word: ok";
  const oc::ChatResult result = ollama.chat(test_model(), {message}, schemas);
  EXPECT_TRUE(result.ok) << result.error;
  registry.shutdown();
}

// The whole point of tool search, live: deferred tools, a model that searches,
// loads `echo` and calls it.
TEST_F(McpIntegrationTest, AModelFindsAToolThroughToolSearch) {
  const oc::BasicOllamaClient ollama(ollama_host());
  if (not ollama.show(test_model()).ok) {
    GTEST_SKIP() << "Ollama model '" << test_model() << "' not reachable at " << ollama_host();
  }
  auto registry = std::make_shared<Registry>(options());
  registry->start({config(std::string(R"({"command":"npx","args":["-y",")") + kPackage +
                          R"(","stdio"]})")});
  if (not connected(*registry)) GTEST_SKIP() << "could not start " << kPackage;

  oc::OllamaClient::configure(test_model(), ollama_host());
  oc::OllamaClient::set_num_ctx(0);
  agent::AgentPool::configure(/*max_agents=*/2, /*max_depth=*/0);
  std::mutex mutex;
  std::vector<agent::AgentEvent> events;
  agent::AgentPool::instance().set_observer([&](const agent::AgentEvent& event) {
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(event);
  });

  agent::AgentOptions options;
  options.max_steps = 10;
  options.max_depth = 0;
  options.enable_subagents = false;
  options.enable_bash_repl = false;
  options.enable_bash_search = false;
  options.enable_file_tools = false;
  options.enable_memory = false;
  options.mcp = registry;
  options.tool_search = *ToolSearchSettings::parse("on");
  const policy::YoloPolicy pol;
  const std::string id = agent::AgentPool::instance().register_root("root");
  agent::Agent agent(options, pol, id, "", 0);
  const agent::AgentResult result = agent.run_turn(
      "Use the MCP server named everything to echo back the exact text "
      "'integration-ping'. Its tools are deferred: search for the echo tool first.");
  agent::AgentPool::instance().set_observer({});

  bool searched = false;
  bool echoed = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const agent::AgentEvent& event : events) {
      if (event.kind != agent::AgentEvent::Kind::ToolCall) continue;
      searched = searched or event.tool_name == "tool_search";
      echoed = echoed or event.tool_name == "mcp__everything__echo";
    }
  }
  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(searched) << "the tools were deferred: it had to search";
  EXPECT_TRUE(echoed) << "conclusion: " << result.conclusion;
  registry->shutdown();
}

}  // namespace
}  // namespace mcp
