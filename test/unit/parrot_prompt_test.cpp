// m8trixparrot's system prompt: the recursive coding agent. This is the text
// that used to be Agent::system_prompt(), so these tests are the regression
// net for the one application that kept it.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent/system_prompt.h>
#include <core/agent/workspace_context.h>

#include <parrot_prompt.h>

namespace parrot {
namespace {

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

agent::PromptFacts base_facts() {
  agent::PromptFacts facts;
  facts.max_depth = 3;
  facts.max_agents = 16;
  facts.free_agent_slots = 15;
  facts.tool_names = {"python", "bash", "package_install", "subagent_create",
                      "subagent_wait"};
  facts.enable_python = true;
  facts.enable_package_install = true;
  facts.enable_subagents = true;
  facts.can_spawn_subagents = true;

  // Pin the workspace. workspace_block() embeds this repo's live `git status`,
  // so without this the prompt carries whatever files happen to be modified —
  // and an assertion that some word is absent fails for reasons that have
  // nothing to do with the prompt. (A modified src/core/tools_subagent.cpp is
  // enough to put "subagent" in the text.)
  facts.workspace_cache =
      agent::WorkspaceContext{"/home/ada/work", "/home/ada/work", "main", "",
                              true};
  return facts;
}

TEST(ParrotPromptTest, KeepsTheCodingAgentPreambleAndToolSentence) {
  const std::string prompt = make_system_prompt(base_facts());
  EXPECT_TRUE(has(prompt, "You are a coding agent working in a terminal"));
  EXPECT_TRUE(has(prompt, "5 tools"));
  EXPECT_TRUE(has(prompt, "`python`, `bash`, `package_install`, "
                          "`subagent_create`, and `subagent_wait`"));
}

TEST(ParrotPromptTest, DescribesTheAgentTree) {
  const std::string root = make_system_prompt(base_facts());
  EXPECT_TRUE(has(root, "You are the root agent (depth 0 of max 3)"));
  EXPECT_TRUE(has(root, "15 of 16 agent slots are free"));

  // A subagent inherits the builder, so the same function must serve it.
  agent::PromptFacts child = base_facts();
  child.depth = 1;
  const std::string prompt = make_system_prompt(child);
  EXPECT_TRUE(has(prompt, "You are a subagent at depth 1 of max 3"));
  EXPECT_TRUE(has(prompt, "Your caller sees only your final message"));
}

TEST(ParrotPromptTest, SwitchesTheSubagentRuleOnReachability) {
  agent::PromptFacts off = base_facts();
  off.enable_subagents = false;
  off.can_spawn_subagents = false;
  EXPECT_TRUE(has(make_system_prompt(off), "You have no subagent tools"));

  agent::PromptFacts at_max = base_facts();
  at_max.depth = 3;
  at_max.can_spawn_subagents = false;
  EXPECT_TRUE(has(make_system_prompt(at_max), "at the maximum depth"));

  EXPECT_TRUE(has(make_system_prompt(base_facts()), "`subagent_create` to spawn"));
}

TEST(ParrotPromptTest, SwitchesThePythonRuleOnPackageInstall) {
  EXPECT_TRUE(has(make_system_prompt(base_facts()), "call `package_install`"));

  agent::PromptFacts no_install = base_facts();
  no_install.enable_package_install = false;
  EXPECT_TRUE(has(make_system_prompt(no_install),
                  "you cannot install new ones"));

  agent::PromptFacts no_python = base_facts();
  no_python.enable_python = false;
  EXPECT_TRUE(has(make_system_prompt(no_python),
                  "Use `bash` for all shell operations"));
}

// The gap this refactor closes: m8trixparrot can enable the memory tool from
// settings.json, and the old built-in prompt never mentioned it.
TEST(ParrotPromptTest, MentionsMemoryOnlyWhenItIsOn) {
  EXPECT_FALSE(has(make_system_prompt(base_facts()), "`memory`"));

  agent::PromptFacts on = base_facts();
  on.enable_memory = true;
  on.tool_names.push_back("memory");
  EXPECT_TRUE(has(make_system_prompt(on), "`memory`"));
}

TEST(ParrotPromptTest, MentionsWebSearchOnlyWhenItIsOn) {
  EXPECT_FALSE(has(make_system_prompt(base_facts()), "`websearch`"));

  agent::PromptFacts on = base_facts();
  on.enable_web_search = true;
  on.tool_names.push_back("websearch");
  EXPECT_TRUE(has(make_system_prompt(on), "`websearch`"));
}

TEST(ParrotPromptTest, CarriesTheWorkspaceBlockAndLoopContract) {
  const std::string prompt = make_system_prompt(base_facts());
  EXPECT_TRUE(has(prompt, "Workspace:"));
  EXPECT_TRUE(has(prompt, "- cwd: "));
  EXPECT_TRUE(has(prompt, "Don't retry the identical call"));
}

}  // namespace
}  // namespace parrot
