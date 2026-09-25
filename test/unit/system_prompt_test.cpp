// The per-application prompt seam: core supplies PromptFacts and a minimal
// default, and an application's builder replaces the prompt outright. The
// point of the refactor is that core contributes no text on the override
// path, so that is what these assert.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent/agent.h>
#include <core/agent/agent_pool.h>
#include <core/policy/policy.h>
#include <core/agent/system_prompt.h>

namespace agent {
namespace {

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

PromptFacts facts_with(std::vector<std::string> names) {
  PromptFacts facts;
  facts.tool_names = std::move(names);
  return facts;
}

TEST(SystemPromptTest, JoinsToolNamesForProse) {
  EXPECT_EQ(join_tool_names({}), "");
  EXPECT_EQ(join_tool_names({"bash"}), "`bash`");
  EXPECT_EQ(join_tool_names({"bash", "read"}), "`bash` and `read`");
  EXPECT_EQ(join_tool_names({"a", "b", "c"}), "`a`, `b`, and `c`");
}

TEST(SystemPromptTest, DefaultPromptNamesEveryTool) {
  const PromptFacts facts = facts_with({"python", "bash", "read"});
  const std::string prompt = default_system_prompt(facts);

  for (const std::string& name : facts.tool_names) {
    EXPECT_TRUE(has(prompt, "`" + name + "`")) << name;
  }
  EXPECT_TRUE(has(prompt, "3 tools"));
}

TEST(SystemPromptTest, DefaultPromptCarriesTheLoopContract) {
  const std::string prompt = default_system_prompt(facts_with({"bash"}));
  EXPECT_TRUE(has(prompt, "end the turn with a plain message and no tool call"));
  EXPECT_TRUE(has(prompt, "Don't retry the identical call"));
}

TEST(SystemPromptTest, HasToolAnswersFromTheAdvertisedList) {
  const PromptFacts facts = facts_with({"bash", "memory"});
  EXPECT_TRUE(facts.has_tool("bash"));
  EXPECT_TRUE(facts.has_tool("memory"));
  EXPECT_FALSE(facts.has_tool("python"));
}

// Null catalog and an empty one both mean "say nothing", so a prompt never
// opens a Skills section with nothing under it.
TEST(SystemPromptTest, SkillsBlockIsEmptyWithoutACatalog) {
  PromptFacts facts = facts_with({"bash"});
  EXPECT_EQ(skills_block(facts), "");

  const SkillCatalog empty;
  facts.skills = &empty;
  EXPECT_EQ(skills_block(facts), "");
}

TEST(SystemPromptTest, WorkspaceBlockIsWhatTriggersDetection) {
  const PromptFacts facts = facts_with({"bash"});
  EXPECT_FALSE(facts.workspace_cache.has_value());

  const std::string block = workspace_block(facts);
  EXPECT_TRUE(has(block, "Workspace:"));
  EXPECT_TRUE(has(block, "- cwd: "));
  EXPECT_TRUE(facts.workspace_cache.has_value());
}

// ───────────────── the override path, through a real Agent ─────────────────

TEST(SystemPromptTest, BuilderReplacesThePromptEntirely) {
  constexpr const char* kMarker = "ZZZ_ONLY_THIS_ZZZ";

  AgentOptions options;
  options.system_prompt_builder = [](const PromptFacts&) {
    return std::string(kMarker);
  };

  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 0);

  // Exactly the builder's output: core adds no preamble, no workspace block,
  // no trailing rules.
  EXPECT_EQ(agent.system_prompt(), kMarker);
}

TEST(SystemPromptTest, FallsBackToTheDefaultWithoutABuilder) {
  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(AgentOptions{}, pol, id, "", 0);

  const std::string prompt = agent.system_prompt();
  EXPECT_TRUE(has(prompt, "You are an agent working in a terminal"));
  EXPECT_TRUE(has(prompt, "`bash`"));
}

// The builder is handed the agent's real situation, and is copied into
// subagents, so it must see the depth it was constructed at.
TEST(SystemPromptTest, FactsReportTheAgentsOwnDepthAndTools) {
  AgentOptions options;
  options.enable_python = false;
  options.enable_package_install = false;
  options.enable_subagents = false;
  options.max_depth = 4;

  std::vector<std::string> seen;
  int seen_depth = -1;
  bool seen_can_spawn = true;
  options.system_prompt_builder = [&](const PromptFacts& facts) {
    seen = facts.tool_names;
    seen_depth = facts.depth;
    seen_can_spawn = facts.can_spawn_subagents;
    return std::string("x");
  };

  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(options, pol, id, "", 2);
  agent.system_prompt();

  EXPECT_EQ((std::vector<std::string>{"bash"}), seen);
  EXPECT_EQ(2, seen_depth);
  EXPECT_FALSE(seen_can_spawn);
}

TEST(SystemPromptTest, CanSpawnIsFalseAtMaxDepth) {
  AgentOptions options;
  options.enable_subagents = true;
  options.max_depth = 2;

  bool can_spawn = false;
  options.system_prompt_builder = [&](const PromptFacts& facts) {
    can_spawn = facts.can_spawn_subagents;
    return std::string("x");
  };

  const policy::YoloPolicy pol;
  const std::string id = AgentPool::instance().register_root("root");
  const Agent at_max(options, pol, id, "", 2);
  at_max.system_prompt();
  EXPECT_FALSE(can_spawn);

  const std::string id2 = AgentPool::instance().register_root("root2");
  const Agent below(options, pol, id2, "", 1);
  below.system_prompt();
  EXPECT_TRUE(can_spawn);
}

}  // namespace
}  // namespace agent
