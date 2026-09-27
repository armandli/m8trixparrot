// m8's system prompt: the recursive coding agent whose only execution substrate
// is a persistent shell. The assertions that matter here are the ones covering
// what m8 says that nothing else does — the installed-command block — and the
// two altitudes the one builder has to serve, since AgentOptions is copied by
// value into every subagent.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent/system_prompt.h>
#include <core/agent/workspace_context.h>

#include <m8_paths.h>
#include <m8_prompt.h>

namespace m8 {
namespace {

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

M8Paths base_paths() {
  M8Paths paths;
  paths.root = "/home/ada/work";
  paths.home = "/home/ada";
  paths.in_workspace = true;
  return paths;
}

std::vector<std::string> all_tools() {
  std::vector<std::string> names;
  for (const InstalledTool& tool : known_installed_tools()) {
    names.push_back(tool.name);
  }
  return names;
}

agent::PromptFacts base_facts() {
  agent::PromptFacts facts;
  facts.max_depth = 3;
  facts.max_agents = 16;
  facts.free_agent_slots = 15;
  facts.tool_names = {"bash_repl", "bash_search", "subagent_create",
                      "subagent_wait"};
  facts.enable_bash_repl = true;
  facts.enable_bash_search = true;
  facts.enable_subagents = true;
  facts.can_spawn_subagents = true;

  // Pin the workspace. workspace_block() embeds this repo's live `git status`,
  // so without this the prompt carries whatever files happen to be modified —
  // and an assertion that some word is absent fails for reasons that have
  // nothing to do with the prompt. (A modified subagent_tool.cpp is enough to
  // put "subagent" in the text.)
  facts.workspace_cache =
      agent::WorkspaceContext{"/home/ada/work", "/home/ada/work", "main", "",
                              true};
  return facts;
}

std::string prompt_of(const agent::PromptFacts& facts) {
  return make_system_prompt(facts, base_paths(), all_tools());
}

TEST(M8PromptTest, OpensWithTheCodingAgentRoleAndToolSentence) {
  const std::string prompt = prompt_of(base_facts());
  EXPECT_TRUE(has(prompt, "You are m8, a coding agent working in a terminal"));
  EXPECT_TRUE(has(prompt, "4 tools"));
  EXPECT_TRUE(has(prompt, "`bash_repl`, `bash_search`, `subagent_create`, and "
                          "`subagent_wait`"));
}

TEST(M8PromptTest, ServesBothTheRootAndASubagent) {
  const std::string root = prompt_of(base_facts());
  EXPECT_TRUE(has(root, "You are m8, a coding agent"));
  EXPECT_TRUE(has(root, "or asking the user to run things for you"));

  agent::PromptFacts child = base_facts();
  child.depth = 1;
  const std::string prompt = prompt_of(child);
  EXPECT_TRUE(has(prompt, "You are an m8 subagent at depth 1 of max 3"));
  EXPECT_TRUE(has(prompt, "will read only your final message"));
  // A subagent is told not to ask a human, because there is no human on the
  // other end of it.
  EXPECT_FALSE(has(prompt, "asking the user to run things for you"));
}

// The block that makes m8 different from the other agents in this repo: it has
// no read/write/edit tool, so the prompt has to name the commands that replace
// them or the model will reinvent them out of cat and sed.
TEST(M8PromptTest, ListsEveryInstalledToolItWasGiven) {
  const std::string prompt = prompt_of(base_facts());
  EXPECT_TRUE(has(prompt, "Installed commands:"));
  for (const InstalledTool& tool : known_installed_tools()) {
    EXPECT_TRUE(has(prompt, std::string("`") + tool.name + "`"))
        << tool.name << " is missing from the prompt";
    EXPECT_TRUE(has(prompt, tool.purpose)) << tool.name << "'s purpose";
  }
  EXPECT_TRUE(has(prompt, "--help"));
}

// A command that is not installed must not be advertised: the model would run
// it, get "not found", and have to recover from a claim the prompt made up.
TEST(M8PromptTest, NamesOnlyTheToolsActuallyFound) {
  const std::string prompt =
      make_system_prompt(base_facts(), base_paths(), {"tool_read", "tool_grep"});
  EXPECT_TRUE(has(prompt, "`tool_read`"));
  EXPECT_TRUE(has(prompt, "`tool_grep`"));
  EXPECT_FALSE(has(prompt, "`tool_write`"));
  EXPECT_FALSE(has(prompt, "`tool_websearch`"));
}

TEST(M8PromptTest, OmitsTheInstalledBlockWhenNothingIsInstalled) {
  const std::string prompt = make_system_prompt(base_facts(), base_paths(), {});
  EXPECT_FALSE(has(prompt, "Installed commands:"));
  // The shell is still there, so the prompt is still usable.
  EXPECT_TRUE(has(prompt, "`bash_repl`"));
}

TEST(M8PromptTest, ExplainsThatTheShellOutlivesTheCall) {
  const std::string prompt = prompt_of(base_facts());
  EXPECT_TRUE(has(prompt, "one shell that stays alive across calls"));

  agent::PromptFacts one_shot = base_facts();
  one_shot.enable_bash_repl = false;
  one_shot.tool_names[0] = "bash";
  EXPECT_FALSE(has(prompt_of(one_shot), "stays alive across calls"));
}

TEST(M8PromptTest, DescribesBashSearchOnlyWhenItIsOn) {
  EXPECT_TRUE(has(prompt_of(base_facts()), "Finding other commands:"));

  agent::PromptFacts off = base_facts();
  off.enable_bash_search = false;
  EXPECT_FALSE(has(prompt_of(off), "Finding other commands:"));
}

TEST(M8PromptTest, SwitchesTheDelegationBlockOnReachability) {
  EXPECT_TRUE(has(prompt_of(base_facts()), "Delegating work:"));
  EXPECT_TRUE(has(prompt_of(base_facts()), "15 of 16 agent slots are free"));

  agent::PromptFacts off = base_facts();
  off.enable_subagents = false;
  off.can_spawn_subagents = false;
  EXPECT_FALSE(has(prompt_of(off), "Delegating work:"));
  EXPECT_TRUE(has(prompt_of(off), "You have no subagent tools"));

  // At max depth the tools are still advertised but a call would be refused, so
  // the block goes and the rule explaining why stays.
  agent::PromptFacts at_max = base_facts();
  at_max.depth = 3;
  at_max.can_spawn_subagents = false;
  EXPECT_FALSE(has(prompt_of(at_max), "Delegating work:"));
  EXPECT_TRUE(has(prompt_of(at_max), "at the maximum depth"));
}

// A subagent that can still nest gets the delegation block too: that is how the
// recursion happens, and the tools are advertised to it either way.
TEST(M8PromptTest, GivesANestingSubagentTheDelegationBlock) {
  agent::PromptFacts child = base_facts();
  child.depth = 1;
  const std::string prompt = prompt_of(child);
  EXPECT_TRUE(has(prompt, "Delegating work:"));
  EXPECT_TRUE(has(prompt, "2 more level(s) below you"));
}

TEST(M8PromptTest, MentionsMemoryOnlyWhenItIsOn) {
  EXPECT_FALSE(has(prompt_of(base_facts()), "Memory:"));

  agent::PromptFacts on = base_facts();
  on.enable_memory = true;
  on.tool_names.push_back("memory");
  const std::string prompt = prompt_of(on);
  EXPECT_TRUE(has(prompt, "Memory:"));
  // The store's real path, so the model is not guessing where memory lives.
  EXPECT_TRUE(has(prompt, "/home/ada/work/.m8/vdb/memory.m8db"));
  EXPECT_TRUE(has(prompt, "action='recall'"));
  EXPECT_TRUE(has(prompt, "CONVENTION (<subject>)"));
  EXPECT_TRUE(has(prompt, "LESSON (<subject>)"));
}

// Only the root writes: several agents writing the same subject concurrently
// cannot keep one memory per subject, because none of them sees the others.
TEST(M8PromptTest, ForbidsASubagentFromWritingMemory) {
  agent::PromptFacts child = base_facts();
  child.depth = 1;
  child.enable_memory = true;
  child.tool_names.push_back("memory");
  const std::string prompt = prompt_of(child);
  EXPECT_TRUE(has(prompt, "NEVER call action='remember'"));
  EXPECT_TRUE(has(prompt, "MEMORY:"));
  EXPECT_FALSE(has(prompt, "CONVENTION (<subject>)"));
}

TEST(M8PromptTest, MentionsWebSearchOnlyWhenItIsOn) {
  // websearch is a tool_* binary for m8, so the in-process tool must not appear
  // in the tool sentence; the installed block is where the model hears about it.
  const std::string prompt = prompt_of(base_facts());
  EXPECT_TRUE(has(prompt, "`tool_websearch`"));

  agent::PromptFacts on = base_facts();
  on.enable_web_search = true;
  on.tool_names.push_back("websearch");
  EXPECT_TRUE(has(prompt_of(on), "`websearch`"));
}

TEST(M8PromptTest, CarriesTheWorkspaceBlockAndLoopContract) {
  const std::string prompt = prompt_of(base_facts());
  EXPECT_TRUE(has(prompt, "Workspace:"));
  EXPECT_TRUE(has(prompt, "- cwd: "));
  EXPECT_TRUE(has(prompt, "Don't retry the identical call"));
}

// Running outside a project is legitimate, but .m8 then lands in whatever
// directory m8 was started in — which is almost never what the user meant.
TEST(M8PromptTest, SaysSoWhenThereIsNoWorkspace) {
  EXPECT_FALSE(has(prompt_of(base_facts()), "scratch directory"));

  M8Paths scratch = base_paths();
  scratch.in_workspace = false;
  scratch.root = "/tmp/somewhere";
  const std::string prompt =
      make_system_prompt(base_facts(), scratch, all_tools());
  EXPECT_TRUE(has(prompt, "scratch directory"));
  EXPECT_TRUE(has(prompt, "/tmp/somewhere/.m8"));
}

TEST(M8PromptTest, SkillsBlockAppearsOnlyWithACatalog) {
  EXPECT_FALSE(has(prompt_of(base_facts()), "Skills available"));

  agent::SkillCatalog catalog;
  catalog.skills.push_back(agent::SkillInfo{
      "todo-scan", "Find and report TODO comments.", ".m8/skills/todo-scan",
      ".m8/skills/todo-scan/SKILL.md", {}, false, "", true});
  agent::PromptFacts facts = base_facts();
  facts.skills = &catalog;
  facts.tool_names.push_back("skill");

  const std::string prompt = prompt_of(facts);
  EXPECT_TRUE(has(prompt, "todo-scan"));
  EXPECT_TRUE(has(prompt, "Find and report TODO comments."));
}

}  // namespace
}  // namespace m8
