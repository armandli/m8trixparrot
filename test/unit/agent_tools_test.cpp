// AgentOptions::enable_subagents: with it off, the agent advertises only the
// `python` tool and refuses a subagent call rather than acting on it. Default
// options keep all three tools. tool_names() / tool_schemas() need no network;
// the refusal path is driven through a scripted LoopbackServer.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent.h>
#include <core/agent_pool.h>
#include <core/ollama_client.h>
#include <core/policy.h>
#include <loopback_server.h>

namespace agent {
namespace {

TEST(AgentToolsTest, DefaultOptionsAdvertiseAllThreeTools) {
  const YoloPolicy policy;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(AgentOptions{}, policy, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"python", "subagent_create",
                                      "subagent_wait"}),
            agent.tool_names());
  EXPECT_EQ(3u, agent.tool_schemas().size());
}

TEST(AgentToolsTest, PythonOnlyWhenSubagentsDisabled) {
  AgentOptions options;
  options.enable_subagents = false;

  const YoloPolicy policy;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, policy, id, "", 0);

  EXPECT_EQ((std::vector<std::string>{"python"}), agent.tool_names());
  ASSERT_EQ(1u, agent.tool_schemas().size());
  EXPECT_NE(agent.tool_schemas()[0].find("\"name\":\"python\""),
            std::string::npos);
}

TEST(AgentToolsTest, SubagentCallIsRefusedWhenDisabled) {
  test::LoopbackServer server({
      // call 1: the model tries a subagent anyway.
      R"json({"message":{"role":"assistant","content":"","tool_calls":[{"function":{"name":"subagent_create","arguments":{"objective":"do a thing"}}}]},"done":true,"prompt_eval_count":10,"eval_count":5})json",
      // call 2: it gives up and answers directly.
      R"json({"message":{"role":"assistant","content":"done it myself"},"done":true,"prompt_eval_count":20,"eval_count":4})json",
  });
  OllamaClient::configure("test-model", server.url(""));
  OllamaClient::set_num_ctx(0);

  AgentOptions options;
  options.max_steps = 4;
  options.enable_subagents = false;

  const YoloPolicy policy;
  const std::string id = AgentPool::instance().register_root("root");
  Agent agent(options, policy, id, "", 0);

  const AgentResult result = agent.run_turn("delegate something");

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ("done it myself", result.conclusion);

  bool refused = false;
  for (const ChatMessage& message : agent.transcript()) {
    if (message.role == "tool" and
        message.content.find("no tool named 'subagent_create'") !=
            std::string::npos) {
      refused = true;
    }
  }
  EXPECT_TRUE(refused);
}

}  // namespace
}  // namespace agent
