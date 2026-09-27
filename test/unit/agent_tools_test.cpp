// AgentOptions tool gates: with subagents off, the agent advertises only its
// shell and refuses a subagent call rather than acting on it. Default options
// keep three tools; `enable_file_tools`, `enable_web_search`, `enable_bash_search`
// and `enable_memory` add more when set. tool_names() / tool_schemas() need no
// network; the refusal path is driven through a scripted LoopbackServer.
//
// NOTE: cwd-sensitive. `skills_dir` defaults to .m8/skills, so run from the repo
// root the `skill` tool appears in every list below. Drive this through ctest,
// which runs it from the build tree, rather than the bare binary.

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent/agent.h>
#include <core/agent/agent_pool.h>
#include <core/oc/ollama_client.h>
#include <core/policy/policy.h>
#include <loopback_server.h>

namespace agent {
namespace {

TEST(AgentToolsTest, DefaultOptionsAdvertiseTheShellAndSubagents) {
  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  AgentOptions options;
  options.enable_skills = false;  // see the cwd note above
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"bash", "subagent_create",
                                      "subagent_wait"}),
            agent.tool_names());
  EXPECT_EQ(3u, agent.tool_schemas().size());
}

TEST(AgentToolsTest, TheShellIsTheOneToolNothingCanTakeAway) {
  AgentOptions options;
  options.enable_subagents = false;
  options.enable_skills = false;

  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"bash"}), agent.tool_names());
  ASSERT_EQ(1u, agent.tool_schemas().size());
  EXPECT_NE(agent.tool_schemas()[0].find("\"name\":\"bash\""),
            std::string::npos);
}

// bash_repl replaces bash rather than joining it: an agent handed two shells
// would have to guess which one holds its state.
TEST(AgentToolsTest, BashReplReplacesBashRatherThanAddingToIt) {
  AgentOptions options;
  options.enable_bash_repl = true;
  options.enable_subagents = false;
  options.enable_skills = false;

  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"bash_repl"}), agent.tool_names());
  ASSERT_EQ(1u, agent.tool_schemas().size());
  EXPECT_NE(agent.tool_schemas()[0].find("\"name\":\"bash_repl\""),
            std::string::npos);
}

TEST(AgentToolsTest, BashReplSitsWhereBashDidInTheToolOrder) {
  AgentOptions options;
  options.enable_bash_repl = true;
  options.enable_skills = false;

  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"bash_repl", "subagent_create",
                                      "subagent_wait"}),
            agent.tool_names());
}

// m8's own tool set, asserted as one thing: the shell, command discovery, the
// two subagent tools and memory. No read/write/edit and no websearch — those are
// the installed tool_* binaries, run inside bash_repl.
TEST(AgentToolsTest, M8ToolSet) {
  AgentOptions options;
  options.enable_bash_repl = true;
  options.enable_bash_search = true;
  options.enable_memory = true;
  options.enable_file_tools = false;
  options.enable_web_search = false;
  options.enable_skills = false;

  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"bash_repl", "bash_search", "memory",
                                      "subagent_create", "subagent_wait"}),
            agent.tool_names());
  EXPECT_EQ(5u, agent.tool_schemas().size());
}

TEST(AgentToolsTest, FileToolsAdvertisedOnlyWhenEnabled) {
  const policy::YoloPolicy pol;

  {
    AgentOptions bare;
    bare.enable_skills = false;
    const std::string id = AgentPool::instance().register_root("root");
    const Agent agent(bare, pol, id, "", 0);
    const std::vector<std::string> names = agent.tool_names();
    EXPECT_EQ(names.end(),
              std::find(names.begin(), names.end(), std::string("read")));
  }

  AgentOptions options;
  options.enable_file_tools = true;
  options.enable_skills = false;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"bash", "read", "write", "edit",
                                      "subagent_create", "subagent_wait"}),
            agent.tool_names());
  EXPECT_EQ(6u, agent.tool_schemas().size());
}

TEST(AgentToolsTest, WebSearchAdvertisedOnlyWhenEnabled) {
  const policy::YoloPolicy pol;

  {
    AgentOptions bare;
    bare.enable_skills = false;
    const std::string id = AgentPool::instance().register_root("root");
    const Agent agent(bare, pol, id, "", 0);
    const std::vector<std::string> names = agent.tool_names();
    EXPECT_EQ(names.end(),
              std::find(names.begin(), names.end(), std::string("websearch")));
  }

  AgentOptions options;
  options.enable_web_search = true;
  options.enable_skills = false;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"bash", "websearch", "subagent_create",
                                      "subagent_wait"}),
            agent.tool_names());
  EXPECT_EQ(4u, agent.tool_schemas().size());
}

TEST(AgentToolsTest, AskUserAdvertisedOnlyWithAHandler) {
  const policy::YoloPolicy pol;

  {
    AgentOptions bare;
    bare.enable_skills = false;
    const std::string id = AgentPool::instance().register_root("root");
    const Agent agent(bare, pol, id, "", 0);
    const std::vector<std::string> names = agent.tool_names();
    EXPECT_EQ(names.end(),
              std::find(names.begin(), names.end(), std::string("ask_user")));
  }

  AgentOptions options;
  options.enable_skills = false;
  options.ask_user_handler = [](const std::string&) { return "sure"; };
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  const std::vector<std::string> names = agent.tool_names();
  EXPECT_EQ("ask_user", names.back());
}

TEST(AgentToolsTest, AskUserHandlerReplyIsFedBackToTheModel) {
  m8test::LoopbackServer server({
      // call 1: the model asks the operator something.
      R"json({"message":{"role":"assistant","content":"","tool_calls":[{"function":{"name":"ask_user","arguments":{"prompt":"proceed?"}}}]},"done":true,"prompt_eval_count":10,"eval_count":5})json",
      // call 2: it answers, having seen the reply.
      R"json({"message":{"role":"assistant","content":"acknowledged"},"done":true,"prompt_eval_count":20,"eval_count":4})json",
  });
  oc::OllamaClient::configure("test-model", server.url(""));
  oc::OllamaClient::set_num_ctx(0);

  std::string asked;
  AgentOptions options;
  options.max_steps = 4;
  options.ask_user_handler = [&asked](const std::string& prompt) {
    asked = prompt;
    return std::string("approved");
  };

  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  Agent agent(options, pol, id, "", 0);

  const AgentResult result = agent.run_turn("do the thing");

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ("acknowledged", result.conclusion);
  EXPECT_EQ("proceed?", asked);

  bool relayed = false;
  for (const oc::ChatMessage& message : agent.transcript()) {
    if (message.role == "tool" and message.tool_name == "ask_user" and
        message.content == "approved") {
      relayed = true;
    }
  }
  EXPECT_TRUE(relayed);
}

TEST(AgentToolsTest, SubagentCallIsRefusedWhenDisabled) {
  m8test::LoopbackServer server({
      // call 1: the model tries a subagent anyway.
      R"json({"message":{"role":"assistant","content":"","tool_calls":[{"function":{"name":"subagent_create","arguments":{"objective":"do a thing"}}}]},"done":true,"prompt_eval_count":10,"eval_count":5})json",
      // call 2: it gives up and answers directly.
      R"json({"message":{"role":"assistant","content":"done it myself"},"done":true,"prompt_eval_count":20,"eval_count":4})json",
  });
  oc::OllamaClient::configure("test-model", server.url(""));
  oc::OllamaClient::set_num_ctx(0);

  AgentOptions options;
  options.max_steps = 4;
  options.enable_subagents = false;

  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  Agent agent(options, pol, id, "", 0);

  const AgentResult result = agent.run_turn("delegate something");

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ("done it myself", result.conclusion);

  bool refused = false;
  for (const oc::ChatMessage& message : agent.transcript()) {
    if (message.role == "tool" and
        message.content.find("no tool named 'subagent_create'") !=
            std::string::npos) {
      refused = true;
    }
  }
  EXPECT_TRUE(refused);
}

TEST(AgentToolsTest, MemoryIsAdvertisedOnlyWhenEnabled) {
  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("memory-gate");

  AgentOptions off;
  off.enable_skills = false;
  const Agent without(off, pol, id, "", 0);
  const std::vector<std::string> names = without.tool_names();
  EXPECT_EQ(std::find(names.begin(), names.end(), "memory"), names.end());

  AgentOptions on = off;
  on.enable_memory = true;
  const Agent with(on, pol, id, "", 0);
  const std::vector<std::string> enabled = with.tool_names();
  EXPECT_NE(std::find(enabled.begin(), enabled.end(), "memory"), enabled.end());
  EXPECT_EQ(with.tool_schemas().size(), names.size() + 1);
}

}  // namespace
}  // namespace agent
