// AgentOptions tool gates: with subagents off, the agent advertises only the
// `python` and `bash` tools and refuses a subagent call rather than acting on
// it. Default options keep five tools; `enable_file_tools` and
// `enable_web_search` add more when set. tool_names() / tool_schemas() need
// no network; the refusal path is driven through a scripted LoopbackServer.

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent.h>
#include <core/agent_pool.h>
#include <core/oc/ollama_client.h>
#include <core/policy.h>
#include <loopback_server.h>

namespace agent {
namespace {

TEST(AgentToolsTest, DefaultOptionsAdvertiseAllFiveTools) {
  const YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(AgentOptions{}, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"python", "bash", "package_install",
                                      "subagent_create", "subagent_wait"}),
            agent.tool_names());
  EXPECT_EQ(5u, agent.tool_schemas().size());
}

TEST(AgentToolsTest, PythonAndBashOnlyWhenSubagentsAndPackageInstallDisabled) {
  AgentOptions options;
  options.enable_subagents = false;
  options.enable_package_install = false;

  const YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"python", "bash"}), agent.tool_names());
  ASSERT_EQ(2u, agent.tool_schemas().size());
  EXPECT_NE(agent.tool_schemas()[0].find("\"name\":\"python\""),
            std::string::npos);
  EXPECT_NE(agent.tool_schemas()[1].find("\"name\":\"bash\""),
            std::string::npos);
}

// bash_repl replaces bash rather than joining it: an agent handed two shells
// would have to guess which one holds its state.
TEST(AgentToolsTest, BashReplReplacesBashRatherThanAddingToIt) {
  AgentOptions options;
  options.enable_bash_repl = true;
  options.enable_python = false;
  options.enable_subagents = false;
  options.enable_package_install = false;

  const YoloPolicy pol;
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

  const YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"python", "bash_repl", "package_install",
                                      "subagent_create", "subagent_wait"}),
            agent.tool_names());
}

TEST(AgentToolsTest, FileToolsAdvertisedOnlyWhenEnabled) {
  const YoloPolicy pol;

  {
    const std::string id = AgentPool::instance().register_root("root");
    const Agent agent(AgentOptions{}, pol, id, "", 0);
    const std::vector<std::string> names = agent.tool_names();
    EXPECT_EQ(names.end(),
              std::find(names.begin(), names.end(), std::string("read")));
  }

  AgentOptions options;
  options.enable_file_tools = true;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"python", "bash", "read", "write", "edit",
                                      "package_install", "subagent_create",
                                      "subagent_wait"}),
            agent.tool_names());
  EXPECT_EQ(8u, agent.tool_schemas().size());
}

TEST(AgentToolsTest, WebSearchAdvertisedOnlyWhenEnabled) {
  const YoloPolicy pol;

  {
    const std::string id = AgentPool::instance().register_root("root");
    const Agent agent(AgentOptions{}, pol, id, "", 0);
    const std::vector<std::string> names = agent.tool_names();
    EXPECT_EQ(names.end(),
              std::find(names.begin(), names.end(), std::string("websearch")));
  }

  AgentOptions options;
  options.enable_web_search = true;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"python", "bash", "package_install",
                                      "websearch", "subagent_create",
                                      "subagent_wait"}),
            agent.tool_names());
  EXPECT_EQ(6u, agent.tool_schemas().size());
}

TEST(AgentToolsTest, AskUserAdvertisedOnlyWithAHandler) {
  const YoloPolicy pol;

  {
    const std::string id = AgentPool::instance().register_root("root");
    const Agent agent(AgentOptions{}, pol, id, "", 0);
    const std::vector<std::string> names = agent.tool_names();
    EXPECT_EQ(names.end(),
              std::find(names.begin(), names.end(), std::string("ask_user")));
  }

  AgentOptions options;
  options.ask_user_handler = [](const std::string&) { return "sure"; };
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  const std::vector<std::string> names = agent.tool_names();
  EXPECT_EQ("ask_user", names.back());
}

TEST(AgentToolsTest, AskUserHandlerReplyIsFedBackToTheModel) {
  test::LoopbackServer server({
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

  const YoloPolicy pol;
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
  test::LoopbackServer server({
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

  const YoloPolicy pol;
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
  const YoloPolicy pol;
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
