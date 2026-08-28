// AgentPool: recursion caps, tree assembly from the registry, and the live /
// total counters. No real model — every spawned agent's first chat call goes to
// a dead port and fails fast, so a subagent finishes almost immediately with
// ok == false. That is enough to exercise structure and bookkeeping.
//
// The pool is a process-wide singleton, so state carries between tests: every
// assertion here is relative to a baseline captured at the start of the test,
// and TearDown drains every subagent it spawned.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent.h>
#include <core/agent_pool.h>
#include <core/agent_result.h>
#include <core/ollama_client.h>
#include <core/policy.h>

namespace agent {
namespace {

struct AgentPoolTest : ::testing::Test {
  YoloPolicy policy;
  std::vector<std::string> spawned;

  void SetUp() override {
    OllamaClient::configure("test-model", "http://127.0.0.1:1");
  }

  void TearDown() override {
    for (const std::string& id : spawned) AgentPool::instance().wait_for(id);
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
        AgentPool::instance().spawn(parent, objective, policy, options);
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

}  // namespace
}  // namespace agent
