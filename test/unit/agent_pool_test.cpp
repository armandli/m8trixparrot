// AgentPool: recursion caps, tree assembly from the registry, and the live /
// total counters. Most of these need no model — every spawned agent's first
// chat call goes to a dead port and fails fast, so a subagent finishes almost
// immediately with ok == false. That is enough to exercise structure and
// bookkeeping.
//
// The last two tests are the exception, and they cover the guarantee the rest
// of the recursion rests on: that a spawned agent actually completes a turn and
// hands its conclusion back, and that it inherits the prompt builder its parent
// was configured with. Both drive a scripted LoopbackServer as a fake Ollama.
//
// The pool is a process-wide singleton, so state carries between tests: every
// assertion here is relative to a baseline captured at the start of the test,
// and TearDown drains every subagent it spawned.

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent.h>
#include <core/agent_pool.h>
#include <core/agent_result.h>
#include <core/oc/ollama_client.h>
#include <core/policy/policy.h>
#include <core/tools/tools.h>
#include <loopback_server.h>

namespace agent {
namespace {

struct AgentPoolTest : ::testing::Test {
  policy::YoloPolicy pol;
  std::vector<std::string> spawned;

  void SetUp() override {
    oc::OllamaClient::configure("test-model", "http://127.0.0.1:1");
  }

  void TearDown() override {
    for (const std::string& id : spawned) AgentPool::instance().wait_for(id);
    // Process-wide, like the pool itself: a stale observer would fire into a
    // dead test's locals.
    AgentPool::instance().set_observer({});
  }

  AgentOptions opts(int max_depth, int max_agents) {
    AgentPool::configure(max_agents, max_depth);
    AgentOptions options;
    options.max_depth = max_depth;
    options.max_agents = max_agents;
    return options;
  }

  SpawnResult spawn_under(const std::string& parent,
                          const std::string& objective,
                          const AgentOptions& options) {
    const SpawnResult result =
        AgentPool::instance().spawn(parent, objective, pol, options);
    if (result.ok) spawned.push_back(result.id);
    return result;
  }
};

TEST_F(AgentPoolTest, DepthCapRefusesBeyondMaxDepth) {
  const AgentOptions options = opts(/*max_depth=*/2, /*max_agents=*/64);
  AgentPool& pool = AgentPool::instance();
  const int before = pool.total_spawned();

  const std::string root = pool.register_root("root");
  const SpawnResult d1 = spawn_under(root, "depth 1", options);
  ASSERT_TRUE(d1.ok) << d1.error;
  const SpawnResult d2 = spawn_under(d1.id, "depth 2", options);
  ASSERT_TRUE(d2.ok) << d2.error;
  const SpawnResult d3 = spawn_under(d2.id, "depth 3", options);

  EXPECT_FALSE(d3.ok);
  EXPECT_NE(d3.error.find("depth"), std::string::npos);
  EXPECT_EQ(before + 2, pool.total_spawned());
}

TEST_F(AgentPoolTest, AssembleTreeNestsByRegistry) {
  const AgentOptions options = opts(/*max_depth=*/5, /*max_agents=*/64);
  AgentPool& pool = AgentPool::instance();
  const std::string root = pool.register_root("root");

  const SpawnResult a = spawn_under(root, "A", options);
  ASSERT_TRUE(a.ok) << a.error;
  const SpawnResult b = spawn_under(a.id, "B", options);
  ASSERT_TRUE(b.ok) << b.error;
  const SpawnResult c = spawn_under(root, "C", options);
  ASSERT_TRUE(c.ok) << c.error;

  pool.wait_for(a.id);
  pool.wait_for(b.id);
  pool.wait_for(c.id);

  const AgentResult tree = pool.assemble_tree(root);
  ASSERT_EQ(2u, tree.children.size());
  EXPECT_EQ("A", tree.children[0].objective);
  ASSERT_EQ(1u, tree.children[0].children.size());
  EXPECT_EQ("B", tree.children[0].children[0].objective);
  EXPECT_EQ("C", tree.children[1].objective);
  EXPECT_TRUE(tree.children[1].children.empty());
}

TEST_F(AgentPoolTest, CountersTrackLiveAndTotal) {
  const AgentOptions options = opts(/*max_depth=*/3, /*max_agents=*/64);
  AgentPool& pool = AgentPool::instance();
  const int before_total = pool.total_spawned();
  const std::string root = pool.register_root("root");

  constexpr int kCount = 5;
  std::vector<std::string> ids;
  for (int i = 0; i < kCount; ++i) {
    const SpawnResult s =
        spawn_under(root, "child " + std::to_string(i), options);
    ASSERT_TRUE(s.ok) << s.error;
    ids.push_back(s.id);
  }
  for (const std::string& id : ids) pool.wait_for(id);

  EXPECT_EQ(before_total + kCount, pool.total_spawned());
  EXPECT_EQ(0, pool.live_count());
}

TEST_F(AgentPoolTest, WaitForReturnsTheResult) {
  const AgentOptions options = opts(/*max_depth=*/3, /*max_agents=*/64);
  AgentPool& pool = AgentPool::instance();
  const std::string root = pool.register_root("root");

  const SpawnResult s = spawn_under(root, "compute something", options);
  ASSERT_TRUE(s.ok) << s.error;

  const std::optional<AgentResult> result = pool.wait_for(s.id);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("compute something", result->objective);

  EXPECT_FALSE(pool.wait_for("no-such-id").has_value());
}


// The turn a subagent actually runs. Everything above this point is satisfied
// by an agent that fails instantly, which would not notice if spawn() stopped
// running turns at all.
TEST_F(AgentPoolTest, ASpawnedAgentRunsATurnAndReportsItsConclusion) {
  test::LoopbackServer server({
      R"json({"message":{"role":"assistant","content":"counted 42 files"},"done":true,"prompt_eval_count":11,"eval_count":3})json",
  });
  oc::OllamaClient::configure("test-model", server.url(""));
  oc::OllamaClient::set_num_ctx(0);

  struct Seen {
    std::mutex mutex;
    bool started = false;
    bool finished = false;
    bool finished_ok = false;
  };
  const auto seen = std::make_shared<Seen>();
  AgentPool::instance().set_observer([seen](const AgentEvent& event) {
    std::lock_guard<std::mutex> lock(seen->mutex);
    if (event.kind == AgentEvent::Kind::SubagentStart) seen->started = true;
    if (event.kind == AgentEvent::Kind::SubagentDone) {
      seen->finished = true;
      seen->finished_ok = event.ok;
    }
  });

  AgentOptions options = opts(/*max_depth=*/3, /*max_agents=*/64);
  options.max_steps = 4;
  options.enable_python = false;

  AgentPool& pool = AgentPool::instance();
  const std::string root = pool.register_root("root");
  const SpawnResult spawn = spawn_under(root, "count the files", options);
  ASSERT_TRUE(spawn.ok) << spawn.error;

  // Through the tool the model actually calls, not through the pool directly.
  tools::ToolArgs args;
  args["id"] = spawn.id;
  const tools::ToolResult waited = SubagentWaitTool{}.execute(args);

  ASSERT_TRUE(waited.ok) << waited.error;
  EXPECT_NE(waited.output.find("counted 42 files"), std::string::npos)
      << waited.output;
  EXPECT_NE(waited.output.find("count the files"), std::string::npos)
      << waited.output;
  EXPECT_NE(waited.output.find("\"ok\":true"), std::string::npos)
      << waited.output;

  std::lock_guard<std::mutex> lock(seen->mutex);
  EXPECT_TRUE(seen->started);
  EXPECT_TRUE(seen->finished);
  EXPECT_TRUE(seen->finished_ok);
}

// AgentOptions is copied by value into the child, system_prompt_builder
// included — which is the only reason an application can write one prompt that
// serves the whole tree. If this ever stopped holding, every app's subagents
// would silently fall back to core's default_system_prompt().
TEST_F(AgentPoolTest, ASpawnedAgentInheritsItsParentsPromptBuilder) {
  test::LoopbackServer server({
      R"json({"message":{"role":"assistant","content":"ok"},"done":true,"prompt_eval_count":7,"eval_count":2})json",
  });
  oc::OllamaClient::configure("test-model", server.url(""));
  oc::OllamaClient::set_num_ctx(0);

  struct Calls {
    std::mutex mutex;
    std::vector<int> depths;
  };
  const auto calls = std::make_shared<Calls>();

  AgentOptions options = opts(/*max_depth=*/3, /*max_agents=*/64);
  options.max_steps = 2;
  options.enable_python = false;
  options.system_prompt_builder = [calls](const PromptFacts& facts) {
    std::lock_guard<std::mutex> lock(calls->mutex);
    calls->depths.push_back(facts.depth);
    return "MARKER depth=" + std::to_string(facts.depth);
  };

  AgentPool& pool = AgentPool::instance();
  const std::string root = pool.register_root("root");
  const SpawnResult spawn = spawn_under(root, "inherit the builder", options);
  ASSERT_TRUE(spawn.ok) << spawn.error;
  ASSERT_TRUE(pool.wait_for(spawn.id).has_value());

  std::lock_guard<std::mutex> lock(calls->mutex);
  // The child built its prompt with the parent's builder, and was told its own
  // depth rather than the root's.
  ASSERT_FALSE(calls->depths.empty());
  for (const int depth : calls->depths) EXPECT_EQ(1, depth);
}

}  // namespace
}  // namespace agent
