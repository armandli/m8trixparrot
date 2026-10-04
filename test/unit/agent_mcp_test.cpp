// MCP inside the agent loop: what goes into the tools array each step, how
// tool_search changes it, how calls reach the toolbox, and what survives
// summarization. The model is a scripted fake Ollama that records every
// request, so a test can check the `tools` an actual /api/chat carried; the
// MCP side is a FakeToolbox with no processes behind it.

#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent/agent.h>
#include <core/agent/agent_pool.h>
#include <core/mcp/catalog.h>
#include <core/oc/ollama_client.h>
#include <core/policy/policy.h>
#include <core/util/json_value.h>
#include <http_test_server.h>

namespace agent {
namespace {

using util::JsonValue;

struct FakeToolbox : mcp::Toolbox {
  std::shared_ptr<const mcp::Catalog> catalog;
  mutable std::mutex mutex;
  std::vector<std::pair<std::string, std::string>> calls;  // exposed, raw args

  std::shared_ptr<const mcp::Catalog> snapshot() const override { return catalog; }
  std::shared_ptr<const mcp::Catalog> wait_until_settled(
      std::chrono::milliseconds) override {
    return catalog;
  }
  tools::ToolResult call_tool(const std::string& exposed, std::string_view arguments,
                              const mcp::CallContext&) override {
    std::lock_guard<std::mutex> lock(mutex);
    calls.emplace_back(exposed, std::string(arguments));
    tools::ToolResult result;
    result.ok = true;
    result.output = "result of " + exposed;
    return result;
  }
  tools::ToolResult resource(std::string_view, const std::string&, const std::string&,
                             const mcp::CallContext&) override {
    tools::ToolResult result;
    result.ok = true;
    result.output = "resources";
    return result;
  }
};

mcp::ToolInfo tool(const std::string& name, const std::string& description) {
  mcp::ToolInfo info;
  info.name = name;
  info.description = description;
  info.input_schema = *JsonValue::parse(
      R"({"type":"object","properties":{"message":{"type":"string"},"nested":{"type":"object"}}})");
  return info;
}

std::shared_ptr<FakeToolbox> toolbox(size_t extra_tools = 0, bool resources = false,
                                     mcp::ServerState extra_state = mcp::ServerState::Disabled) {
  mcp::Catalog catalog;
  mcp::CatalogServer server;
  server.name = "fake";
  server.state = mcp::ServerState::Connected;
  server.capabilities.resources = resources;
  server.instructions = "Use echo to echo things.";
  catalog.servers.push_back(server);
  if (extra_state != mcp::ServerState::Disabled) {
    mcp::CatalogServer other;
    other.name = "broken";
    other.state = extra_state;
    catalog.servers.push_back(other);
  }
  std::vector<mcp::ToolInfo> infos = {tool("echo", "Echo a message back"),
                                      tool("add", "Add two numbers")};
  for (size_t i = 0; i < extra_tools; ++i) {
    infos.push_back(tool("tool_" + std::to_string(i), "Generated tool"));
  }
  for (const mcp::ToolInfo& info : infos) {
    mcp::CatalogTool built;
    std::string problem;
    EXPECT_TRUE(mcp::make_catalog_tool("fake", info, false, false, built, problem));
    catalog.tools.push_back(built);
  }
  auto box = std::make_shared<FakeToolbox>();
  box->catalog = mcp::finalize_catalog(std::move(catalog));
  return box;
}

bool has_tool(const std::vector<std::string>& schemas, const std::string& name) {
  for (const std::string& schema : schemas) {
    if (schema.find("\"name\":\"" + name + "\"") != std::string::npos) return true;
  }
  return false;
}

// The names in one recorded /api/chat request's tools array.
std::vector<std::string> request_tools(const m8test::HttpRequest& request) {
  std::vector<std::string> names;
  const std::optional<JsonValue> body = JsonValue::parse(request.body);
  if (not body) return names;
  for (const JsonValue& entry : body->get("tools").items()) {
    names.push_back(entry.get("function").get("name").as_string());
  }
  return names;
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

// Answers /api/chat with the scripted replies in order (the last repeats).
struct ScriptedOllama {
  std::vector<std::pair<int, std::string>> replies;
  std::mutex mutex;
  size_t next = 0;
  m8test::HttpTestServer server;

  explicit ScriptedOllama(std::vector<std::pair<int, std::string>> script)
      : replies(std::move(script)), server([this](const m8test::HttpRequest&) {
          std::lock_guard<std::mutex> lock(mutex);
          const auto& [status, body] = replies[std::min(next, replies.size() - 1)];
          ++next;
          m8test::HttpReply reply;
          reply.status = status;
          reply.body = body;
          return reply;
        }) {
    oc::OllamaClient::configure("test-model", server.url(""));
    oc::OllamaClient::set_num_ctx(0);
  }

  std::vector<m8test::HttpRequest> chats() const {
    std::vector<m8test::HttpRequest> out;
    for (const m8test::HttpRequest& request : server.requests()) {
      if (request.path == "/api/chat") out.push_back(request);
    }
    return out;
  }
};

std::pair<int, std::string> calls_tool(const std::string& name, const std::string& arguments) {
  return {200, R"({"message":{"role":"assistant","content":"","tool_calls":[{"function":{"name":")" +
                   name + R"(","arguments":)" + arguments +
                   R"(}}]},"done":true,"prompt_eval_count":10,"eval_count":5})"};
}

std::pair<int, std::string> answers(const std::string& text) {
  return {200, R"({"message":{"role":"assistant","content":")" + text +
                   R"("},"done":true,"prompt_eval_count":20,"eval_count":4})"};
}

AgentOptions mcp_options(std::shared_ptr<FakeToolbox> box, const std::string& mode) {
  AgentOptions options;
  options.enable_skills = false;
  options.enable_subagents = false;
  options.max_steps = 6;
  options.mcp = std::move(box);
  options.tool_search = *mcp::ToolSearchSettings::parse(mode);
  return options;
}

// ───────────────────────────── the tools array ──────────────────────────────

TEST(AgentMcpTest, UndeferredToolsAreAllOfferedAndToolSearchIsNot) {
  const policy::YoloPolicy pol;
  const Agent agent(mcp_options(toolbox(), "off"), pol,
                    AgentPool::instance().register_root("root"), "", 0);
  const std::vector<std::string> schemas = agent.tool_schemas();
  EXPECT_TRUE(has_tool(schemas, "mcp__fake__echo"));
  EXPECT_TRUE(has_tool(schemas, "mcp__fake__add"));
  EXPECT_FALSE(has_tool(schemas, "tool_search"));
  // The names list stays readable; MCP tools are summarised in the prompt.
  EXPECT_FALSE(contains(agent.tool_names(), "mcp__fake__echo"));
  EXPECT_NE(std::string::npos, agent.system_prompt().find("2 MCP tools"));
}

TEST(AgentMcpTest, DeferredToolsAreReplacedByToolSearchAndACatalog) {
  const policy::YoloPolicy pol;
  const Agent agent(mcp_options(toolbox(), "on"), pol,
                    AgentPool::instance().register_root("root"), "", 0);
  const std::vector<std::string> schemas = agent.tool_schemas();
  EXPECT_TRUE(has_tool(schemas, "tool_search"));
  EXPECT_FALSE(has_tool(schemas, "mcp__fake__echo"));

  const std::string prompt = agent.system_prompt();
  EXPECT_NE(std::string::npos, prompt.find("fake: echo, add")) << prompt;
  EXPECT_NE(std::string::npos, prompt.find("Use echo to echo things.")) << prompt;
  EXPECT_NE(std::string::npos, prompt.find("written by the server")) << prompt;
}

TEST(AgentMcpTest, AutoDefersOnlyPastTheThreshold) {
  const policy::YoloPolicy pol;
  const Agent small(mcp_options(toolbox(), "auto"), pol,
                    AgentPool::instance().register_root("root"), "", 0);
  EXPECT_FALSE(has_tool(small.tool_schemas(), "tool_search"));

  // More than 30 tools defer whatever they cost.
  const Agent many(mcp_options(toolbox(40), "auto"), pol,
                   AgentPool::instance().register_root("root"), "", 0);
  EXPECT_TRUE(has_tool(many.tool_schemas(), "tool_search"));
}

TEST(AgentMcpTest, ResourcesAndUnavailableServersAreMentioned) {
  const policy::YoloPolicy pol;
  const Agent agent(mcp_options(toolbox(0, true, mcp::ServerState::Failed), "off"), pol,
                    AgentPool::instance().register_root("root"), "", 0);
  EXPECT_TRUE(has_tool(agent.tool_schemas(), "mcp_resource"));
  const std::string prompt = agent.system_prompt();
  EXPECT_NE(std::string::npos, prompt.find("broken: unavailable (failed)")) << prompt;
  EXPECT_NE(std::string::npos, prompt.find("mcp_resource")) << prompt;
}

TEST(AgentMcpTest, WithoutAToolboxNothingChanges) {
  AgentOptions options;
  options.enable_skills = false;
  const policy::YoloPolicy pol;
  const Agent agent(options, pol, AgentPool::instance().register_root("root"), "", 0);
  EXPECT_EQ((std::vector<std::string>{"bash", "subagent_create", "subagent_wait"}),
            agent.tool_names());
  EXPECT_EQ(std::string::npos, agent.system_prompt().find("MCP"));
}

// ──────────────────────────────── the loop ──────────────────────────────────

TEST(AgentMcpTest, AToolLoadedAtOneStepIsOfferedAtTheNext) {
  ScriptedOllama ollama({
      calls_tool("tool_search", R"({"query":"select:echo"})"),
      calls_tool("mcp__fake__echo", R"({"message":"hi","nested":{"a":[1,2]}})"),
      answers("done"),
  });
  auto box = toolbox();
  const policy::YoloPolicy pol;
  Agent agent(mcp_options(box, "on"), pol, AgentPool::instance().register_root("root"), "", 0);
  const AgentResult result = agent.run_turn("echo hi");
  ASSERT_TRUE(result.ok) << result.error;

  const std::vector<m8test::HttpRequest> chats = ollama.chats();
  ASSERT_EQ(3u, chats.size());
  EXPECT_FALSE(contains(request_tools(chats[0]), "mcp__fake__echo"));
  EXPECT_TRUE(contains(request_tools(chats[0]), "tool_search"));
  EXPECT_TRUE(contains(request_tools(chats[1]), "mcp__fake__echo"));

  // The nested arguments arrived intact, which ToolArgs could not have done.
  ASSERT_EQ(1u, box->calls.size());
  EXPECT_EQ("mcp__fake__echo", box->calls[0].first);
  const JsonValue arguments = *JsonValue::parse(box->calls[0].second);
  EXPECT_EQ(2, arguments.get("nested").get("a").items()[1].as_int());

  EXPECT_EQ((std::vector<std::string>{"mcp__fake__echo"}), agent.loaded_mcp_tools());
  EXPECT_TRUE(agent.mcp_deferred());
}

TEST(AgentMcpTest, CallingAnUnloadedToolByNameLoadsAndRunsIt) {
  ScriptedOllama ollama({calls_tool("mcp__fake__add", R"({"message":"x"})"), answers("ok")});
  auto box = toolbox();
  const policy::YoloPolicy pol;
  Agent agent(mcp_options(box, "on"), pol, AgentPool::instance().register_root("root"), "", 0);
  ASSERT_TRUE(agent.run_turn("add").ok);
  ASSERT_EQ(1u, box->calls.size());
  EXPECT_EQ((std::vector<std::string>{"mcp__fake__add"}), agent.loaded_mcp_tools());
}

TEST(AgentMcpTest, ABareToolNameReachesItsServer) {
  ScriptedOllama ollama({calls_tool("echo", R"({"message":"bare"})"), answers("ok")});
  auto box = toolbox();
  const policy::YoloPolicy pol;
  Agent agent(mcp_options(box, "off"), pol, AgentPool::instance().register_root("root"), "", 0);
  ASSERT_TRUE(agent.run_turn("echo").ok);
  ASSERT_EQ(1u, box->calls.size());
  EXPECT_EQ("mcp__fake__echo", box->calls[0].first);

  bool relayed = false;
  for (const oc::ChatMessage& message : agent.transcript()) {
    relayed = relayed or (message.role == "tool" and
                          message.content == "result of mcp__fake__echo");
  }
  EXPECT_TRUE(relayed);
}

TEST(AgentMcpTest, ToolSearchIsCappedPerTurn) {
  std::vector<std::pair<int, std::string>> script;
  for (int i = 0; i < 9; ++i) script.push_back(calls_tool("tool_search", R"({"query":"echo"})"));
  script.push_back(answers("gave up"));
  ScriptedOllama ollama(std::move(script));
  AgentOptions options = mcp_options(toolbox(), "on");
  options.max_steps = 12;
  const policy::YoloPolicy pol;
  Agent agent(options, pol, AgentPool::instance().register_root("root"), "", 0);
  ASSERT_TRUE(agent.run_turn("search a lot").ok);

  int refused = 0;
  for (const oc::ChatMessage& message : agent.transcript()) {
    if (message.role == "tool" and
        message.content.find("already called 8 times") != std::string::npos) {
      ++refused;
    }
  }
  EXPECT_EQ(1, refused);
}

TEST(AgentMcpTest, OllamaRefusingTheSchemasCostsANoticeNotTheTurn) {
  ScriptedOllama ollama({{400, R"({"error":"json: cannot unmarshal array"})"}, answers("fine")});
  std::vector<std::string> notices;
  std::mutex notices_mutex;
  AgentPool::instance().set_observer([&](const AgentEvent& event) {
    std::lock_guard<std::mutex> lock(notices_mutex);
    if (event.kind == AgentEvent::Kind::Notice) notices.push_back(event.text);
  });
  const policy::YoloPolicy pol;
  Agent agent(mcp_options(toolbox(), "off"), pol,
              AgentPool::instance().register_root("root"), "", 0);
  const AgentResult result = agent.run_turn("hello");
  AgentPool::instance().set_observer({});
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("fine", result.conclusion);

  const std::vector<m8test::HttpRequest> chats = ollama.chats();
  ASSERT_EQ(2u, chats.size());
  EXPECT_TRUE(contains(request_tools(chats[0]), "mcp__fake__echo"));
  EXPECT_FALSE(contains(request_tools(chats[1]), "mcp__fake__echo"));
  bool noticed = false;
  for (const std::string& notice : notices) {
    noticed = noticed or notice.find("rejected") != std::string::npos;
  }
  EXPECT_TRUE(noticed);
}

TEST(AgentMcpTest, SummarizationKeepsLoadedToolsAndNamesThem) {
  ScriptedOllama ollama({
      {200, R"({"message":{"role":"assistant","content":"","tool_calls":[{"function":{"name":"tool_search","arguments":{"query":"select:echo"}}}]},"done":true,"prompt_eval_count":999999,"eval_count":5})"},
      answers("SUMMARY"),
      answers("done"),
  });
  AgentOptions options = mcp_options(toolbox(), "on");
  options.context_summarize_at_tokens = 100;
  const policy::YoloPolicy pol;
  Agent agent(options, pol, AgentPool::instance().register_root("root"), "", 0);
  ASSERT_TRUE(agent.run_turn("load and continue").ok);

  EXPECT_EQ((std::vector<std::string>{"mcp__fake__echo"}), agent.loaded_mcp_tools());
  bool named = false;
  for (const oc::ChatMessage& message : agent.transcript()) {
    named = named or message.content.find("MCP tools still loaded: mcp__fake__echo") !=
                         std::string::npos;
  }
  EXPECT_TRUE(named);
}

TEST(AgentMcpTest, ASubagentStartsWithItsParentsLoadedTools) {
  AgentOptions options = mcp_options(toolbox(), "on");
  options.mcp_initial_tools = {"mcp__fake__add"};
  const policy::YoloPolicy pol;
  const Agent child(options, pol, AgentPool::instance().register_root("root"), "", 1);
  EXPECT_EQ((std::vector<std::string>{"mcp__fake__add"}), child.loaded_mcp_tools());
  EXPECT_TRUE(has_tool(child.tool_schemas(), "mcp__fake__add"));
  EXPECT_FALSE(has_tool(child.tool_schemas(), "mcp__fake__echo"));
}

TEST(AgentMcpTest, ResetForgetsLoadedTools) {
  AgentOptions options = mcp_options(toolbox(), "on");
  options.mcp_initial_tools = {"mcp__fake__add"};
  const policy::YoloPolicy pol;
  Agent agent(options, pol, AgentPool::instance().register_root("root"), "", 0);
  agent.reset();
  EXPECT_TRUE(agent.loaded_mcp_tools().empty());
}

}  // namespace
}  // namespace agent
