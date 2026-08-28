// Automatic context summarization: when the transcript's token count crosses
// the threshold, the agent loop replaces the whole history with a one-message
// summary produced by an Ollama call, emits a ContextSummarized event, and
// keeps going. A sequenced LoopbackServer scripts the three model calls this
// takes.

#include <algorithm>
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

TEST(AgentContextTest, SummarizesWhenTranscriptExceedsThreshold) {
  test::LoopbackServer server({
      // call 1: a tool call keeps the loop going; prompt_eval_count is huge.
      R"json({"message":{"role":"assistant","content":"working","tool_calls":[{"function":{"name":"python","arguments":{"script":"print(1)"}}}]},"done":true,"prompt_eval_count":999999,"eval_count":5})json",
      // call 2: the summary.
      R"json({"message":{"role":"assistant","content":"COMPACTED STATE: the user asked for the thing; step 1 done"},"done":true,"prompt_eval_count":42,"eval_count":10})json",
      // call 3: a final answer ends the turn.
      R"json({"message":{"role":"assistant","content":"all done"},"done":true,"prompt_eval_count":50,"eval_count":8})json",
  });
  OllamaClient::configure("test-model", server.url(""));
  OllamaClient::set_num_ctx(0);

  std::vector<AgentEvent::Kind> kinds;
  AgentPool::instance().set_observer(
      [&](const AgentEvent& event) { kinds.push_back(event.kind); });

  AgentOptions options;
  options.max_steps = 4;
  options.context_summarize_at_tokens = 100;  // call 1's 999999 trips it
  options.context_window_tokens = 0;

  const YoloPolicy policy;
  const std::string id = AgentPool::instance().register_root("root");
  Agent root(options, policy, id, "", 0);

  const AgentResult result = root.run_turn("do the thing");

  AgentPool::instance().set_observer({});

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ("all done", result.conclusion);

  // A summarize fired.
  EXPECT_NE(std::find(kinds.begin(), kinds.end(),
                      AgentEvent::Kind::ContextSummarized),
            kinds.end());

  // The transcript was compacted: short, and it holds the summary text.
  EXPECT_LT(root.transcript().size(), 4u);
  bool has_summary = false;
  for (const ChatMessage& message : root.transcript()) {
    if (message.content.find("COMPACTED STATE") != std::string::npos) {
      has_summary = true;
    }
  }
  EXPECT_TRUE(has_summary);
}

TEST(AgentContextTest, NoSummarizeBelowThreshold) {
  test::LoopbackServer server({
      R"json({"message":{"role":"assistant","content":"here is the answer"},"done":true,"prompt_eval_count":500,"eval_count":4})json",
  });
  OllamaClient::configure("test-model", server.url(""));
  OllamaClient::set_num_ctx(0);

  std::vector<AgentEvent::Kind> kinds;
  AgentPool::instance().set_observer(
      [&](const AgentEvent& event) { kinds.push_back(event.kind); });

  AgentOptions options;
  options.max_steps = 3;
  options.context_summarize_at_tokens = 200000;
  options.context_window_tokens = 0;

  const YoloPolicy policy;
  const std::string id = AgentPool::instance().register_root("root");
  Agent root(options, policy, id, "", 0);

  const AgentResult result = root.run_turn("answer me");

  AgentPool::instance().set_observer({});

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ(std::find(kinds.begin(), kinds.end(),
                      AgentEvent::Kind::ContextSummarized),
            kinds.end());
  // A ContextUsage event carried the real count.
  EXPECT_NE(std::find(kinds.begin(), kinds.end(),
                      AgentEvent::Kind::ContextUsage),
            kinds.end());
  EXPECT_EQ(500, root.context_tokens());
}

}  // namespace
}  // namespace agent
