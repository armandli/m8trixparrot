// m8trixsh's system prompt — its first coverage. It used to be an
// anonymous-namespace function in main.cpp, appended after a coding-agent
// preamble that contradicted its opening sentence; it is now the whole prompt
// and lives somewhere a test can reach.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/system_prompt.h>

#include <sh_prompt.h>

namespace sh {
namespace {

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// m8trixsh's real shape: python and file tools on, ask_user wired, subagents
// off unless ~/.m8shrc says otherwise.
agent::PromptFacts base_facts() {
  agent::PromptFacts facts;
  facts.max_depth = 3;
  facts.max_agents = 16;
  facts.free_agent_slots = 16;
  facts.tool_names = {"python", "bash",      "read",   "write",
                      "edit",   "package_install", "ask_user"};
  facts.enable_python = true;
  facts.enable_package_install = true;
  facts.enable_file_tools = true;
  facts.ask_user_offered = true;
  return facts;
}

TEST(ShPromptTest, OpensAsTheShellsAiSideAndNotACodingAgent) {
  const std::string prompt = make_system_prompt(base_facts(), "/home/ada");
  EXPECT_TRUE(has(prompt, "You are the AI side of an interactive shell"));
  EXPECT_FALSE(has(prompt, "coding agent"));
}

TEST(ShPromptTest, KeepsTheApprovalWorkflow) {
  const std::string prompt = make_system_prompt(base_facts(), "/home/ada");
  EXPECT_TRUE(has(prompt, "`ask_user`"));
  EXPECT_TRUE(has(prompt, "/home/ada/bin/<name>.sh"));
  EXPECT_TRUE(has(prompt, "Never run the destructive steps before the script "
                          "is approved"));
  // All five numbered steps survive the move out of main.cpp.
  for (const char* step : {"1. Research first", "2. Call `ask_user`",
                           "3. On approval", "4. Call `ask_user` again",
                           "5. On approval"}) {
    EXPECT_TRUE(has(prompt, step)) << step;
  }
}

TEST(ShPromptTest, NamesItsToolsAndKeepsTheLoopContract) {
  const std::string prompt = make_system_prompt(base_facts(), "/home/ada");
  EXPECT_TRUE(has(prompt, "`read`"));
  EXPECT_TRUE(has(prompt, "`write`"));
  EXPECT_TRUE(has(prompt, "`edit`"));
  EXPECT_TRUE(has(prompt, "Don't retry the identical call"));
}

// Telling the model about subagents it cannot spawn is the kind of noise this
// refactor exists to remove.
TEST(ShPromptTest, MentionsSubagentsOnlyWhenItCanSpawnThem) {
  const std::string off = make_system_prompt(base_facts(), "/home/ada");
  EXPECT_FALSE(has(off, "agent slots are free"));
  EXPECT_FALSE(has(off, "subagent"));

  agent::PromptFacts on = base_facts();
  on.enable_subagents = true;
  on.can_spawn_subagents = true;
  on.tool_names.push_back("subagent_create");
  const std::string prompt = make_system_prompt(on, "/home/ada");
  EXPECT_TRUE(has(prompt, "16 of 16 agent slots are free"));
  EXPECT_TRUE(has(prompt, "`subagent_create`"));
}

TEST(ShPromptTest, MentionsMemoryOnlyWhenItIsOn) {
  EXPECT_FALSE(has(make_system_prompt(base_facts(), "/home/ada"), "`memory`"));

  agent::PromptFacts on = base_facts();
  on.enable_memory = true;
  on.tool_names.push_back("memory");
  EXPECT_TRUE(has(make_system_prompt(on, "/home/ada"), "`memory`"));
}

// A shell in a directory genuinely wants to know which directory.
TEST(ShPromptTest, CarriesTheWorkspaceBlock) {
  const agent::PromptFacts facts = base_facts();
  const std::string prompt = make_system_prompt(facts, "/home/ada");
  EXPECT_TRUE(has(prompt, "Workspace:"));
  EXPECT_TRUE(has(prompt, "- cwd: "));
  EXPECT_TRUE(facts.workspace_cache.has_value());
}

}  // namespace
}  // namespace sh
