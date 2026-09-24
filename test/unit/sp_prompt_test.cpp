// sp's system prompt. sp is a shell assistant, so the tests assert both what
// the prompt says and what it no longer says: none of the coding-agent text
// the one built-in prompt used to force on every application.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/system_prompt.h>

#include <sp_memory.h>
#include <sp_prompt.h>
#include <sp_paths.h>

namespace sp {
namespace {

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// sp's real tool set: bash + bash_search, plus memory when it is on.
agent::PromptFacts facts_for(bool memory) {
  agent::PromptFacts facts;
  facts.depth = 0;
  facts.max_depth = 1;
  facts.max_agents = 1;
  facts.free_agent_slots = 1;
  facts.tool_names = {"bash_repl", "bash_search"};
  if (memory) facts.tool_names.push_back("memory");
  facts.enable_bash_search = true;
  facts.enable_bash_repl = true;
  facts.enable_memory = memory;
  return facts;
}

// sp's XDG layout, with its bin directory already on PATH.
SpPaths paths_on_path() {
  SpPaths paths;
  paths.data = "/home/ada/.local/share/sp";
  paths.config = "/home/ada/.config/sp";
  paths.state = "/home/ada/.local/state/sp";
  paths.cache = "/home/ada/.cache/sp";
  paths.bin_on_path = true;
  return paths;
}

std::string prompt_for(bool memory) {
  const agent::PromptFacts facts = facts_for(memory);
  return make_system_prompt(facts, "ada", "/home/ada", paths_on_path());
}

TEST(SpPromptTest, KeepsTheShellRulesWhicheverWayMemoryGoes) {
  for (const bool memory : {false, true}) {
    const std::string prompt = prompt_for(memory);
    EXPECT_TRUE(has(prompt, "/home/ada/.local/share/sp/trash")) << memory;
    EXPECT_TRUE(has(prompt, "/home/ada/.local/share/sp/bin")) << memory;
    EXPECT_TRUE(has(prompt, "NEVER use rm")) << memory;
    EXPECT_TRUE(has(prompt, "User: ada")) << memory;
  }
}

TEST(SpPromptTest, NamesItselfAndItsTools) {
  const std::string prompt = prompt_for(false);
  EXPECT_TRUE(has(prompt, "You are shell-parrot (sp)"));
  EXPECT_TRUE(has(prompt, "`bash_repl`"));
  EXPECT_TRUE(has(prompt, "`bash_search`"));
}

// A model that assumes each call starts fresh writes self-contained one-liners
// and the persistent shell buys nothing, so the prompt has to say it persists.
TEST(SpPromptTest, ExplainsThatTheShellKeepsItsState) {
  const std::string prompt = prompt_for(false);
  EXPECT_TRUE(has(prompt, "ONE shell that stays alive"));
  EXPECT_TRUE(has(prompt, "still there on your next call"));
  EXPECT_TRUE(has(prompt, "restart=true"));
  // The background-output trap, which would otherwise corrupt a later call.
  EXPECT_TRUE(has(prompt, "/tmp/log 2>&1 &"));
}

TEST(SpPromptTest, SaysNothingAboutAPersistentShellWhenItIsOff) {
  agent::PromptFacts facts = facts_for(false);
  facts.enable_bash_repl = false;
  facts.tool_names = {"bash", "bash_search"};
  const std::string prompt = make_system_prompt(facts, "ada", "/home/ada", paths_on_path());

  EXPECT_FALSE(has(prompt, "Your shell:"));
  EXPECT_FALSE(has(prompt, "restart=true"));
}

// The regression that matters most: an agent that was not given the tool must
// never be told to call it.
TEST(SpPromptTest, SaysNothingAboutMemoryWhenItIsOff) {
  const std::string prompt = prompt_for(false);
  EXPECT_FALSE(has(prompt, "memory"));
  EXPECT_FALSE(has(prompt, "recall"));
  EXPECT_FALSE(has(prompt, "LESSON"));
}

TEST(SpPromptTest, TeachesBothMemoryShapesWhenItIsOn) {
  const std::string prompt = prompt_for(true);
  // The preference shape.
  EXPECT_TRUE(has(prompt, "PREFERENCE (<subject>):"));
  EXPECT_TRUE(has(prompt, "type='semantic'"));
  // The lesson shape: the mistake, how to spot it, what to do, how to avoid it.
  EXPECT_TRUE(has(prompt, "LESSON (<subject>):"));
  EXPECT_TRUE(has(prompt, "Detect:"));
  EXPECT_TRUE(has(prompt, "Instead:"));
  EXPECT_TRUE(has(prompt, "Avoid:"));
  EXPECT_TRUE(has(prompt, "type='procedural'"));
  // Recall first, de-duplicate, and don't hoard junk.
  EXPECT_TRUE(has(prompt, "action='recall'"));
  EXPECT_TRUE(has(prompt, "action='forget'"));
  EXPECT_TRUE(has(prompt, "Never use type='episodic'"));
}

TEST(SpPromptTest, PutsTheMemoryRulesAfterTheShellRules) {
  const std::string prompt = prompt_for(true);
  EXPECT_LT(prompt.find("Rules:"), prompt.find("Memory:"));
}

// ────────────────────────── reusable scripts ───────────────────────────────

TEST(SpPromptTest, NamesTheResolvedScriptAndSourceDirectories) {
  const std::string prompt = prompt_for(false);
  EXPECT_TRUE(has(prompt, "/home/ada/.local/share/sp/bin"));
  EXPECT_TRUE(has(prompt, "/home/ada/.local/share/sp/src"));
  // Everything sp owns now lives under one XDG data root.
  EXPECT_FALSE(has(prompt, ".local/sp_development"));
  EXPECT_FALSE(has(prompt, ".local/share/Trash"));
  EXPECT_FALSE(has(prompt, ".local/share/sp/scripts"));
}

// sp has no `write` tool, so a heredoc is the only way it can create a file —
// and an unquoted one would bake this run's values into the script.
TEST(SpPromptTest, TellsTheModelToUseAQuotedHeredoc) {
  const std::string prompt = prompt_for(false);
  EXPECT_TRUE(has(prompt, "<<'EOF'"));
  EXPECT_TRUE(has(prompt, "quotes around 'EOF' are not optional"));
}

TEST(SpPromptTest, DemandsACommandShapedScript) {
  const std::string prompt = prompt_for(false);
  EXPECT_TRUE(has(prompt, "chmod +x"));
  EXPECT_TRUE(has(prompt, "No extension"));
  EXPECT_TRUE(has(prompt, "#!/usr/bin/env bash"));
  EXPECT_TRUE(has(prompt, "#!/usr/bin/env python3"));
  EXPECT_TRUE(has(prompt, "--help"));
  EXPECT_TRUE(has(prompt, "do not hardcode this run's values"));
}

TEST(SpPromptTest, SaysWhereCppSourceAndItsBinaryGo) {
  const std::string prompt = prompt_for(false);
  EXPECT_TRUE(has(prompt, "/home/ada/.local/share/sp/src/<name>.cpp"));
  EXPECT_TRUE(has(prompt, "command -v c++"));
}

// Installing it is only half the job; the point is the user running it later.
TEST(SpPromptTest, RequiresTheClosingSummaryToExplainTheScript) {
  const std::string prompt = prompt_for(false);
  EXPECT_TRUE(has(prompt, "Telling the user about a script:"));
  EXPECT_TRUE(has(prompt, "the full path you wrote it to"));
  EXPECT_TRUE(has(prompt, "copy and paste"));
}

// Only a user whose PATH actually needs fixing should be told to edit their rc
// file — and one whose PATH is fine should never be nagged about it.
TEST(SpPromptTest, ExplainsThePathFixOnlyWhenTheDirectoryIsNotOnPath) {
  const std::string fine = prompt_for(false);
  EXPECT_TRUE(has(fine, "already on the user's PATH"));
  EXPECT_FALSE(has(fine, "export PATH="));
  EXPECT_FALSE(has(fine, ".zshrc"));

  SpPaths off = paths_on_path();
  off.bin_on_path = false;
  const std::string prompt =
      make_system_prompt(facts_for(false), "ada", "/home/ada", off);

  EXPECT_TRUE(has(prompt, "NOT on the user's PATH"));
  EXPECT_TRUE(has(prompt,
                  "export PATH=\"/home/ada/.local/share/sp/bin:$PATH\""));
  EXPECT_TRUE(has(prompt, ".zshrc"));
}

TEST(SpPromptTest, InstallsOnlyWhatIsWorthKeeping) {
  const std::string prompt = prompt_for(false);
  EXPECT_TRUE(has(prompt, "would plausibly run again"));
  EXPECT_TRUE(has(prompt, "one-off step"));
}

// ───────────────────── what sp no longer carries ──────────────────────

TEST(SpPromptTest, CarriesNoCodingAgentPreamble) {
  const std::string prompt = prompt_for(true);
  EXPECT_FALSE(has(prompt, "coding agent"));
  // One role definition, not two competing ones.
  EXPECT_EQ(prompt.find("You are"), prompt.rfind("You are shell-parrot"));
}

TEST(SpPromptTest, CarriesNoAgentTreeOrSubagentText) {
  const std::string prompt = prompt_for(true);
  EXPECT_FALSE(has(prompt, "agent slots are free"));
  EXPECT_FALSE(has(prompt, "subagent"));
  EXPECT_FALSE(has(prompt, "depth"));
}

// The lazy-workspace guarantee: sp's builder never asks for the workspace, so
// the git shell-out never happens — and it used to happen on every model call.
TEST(SpPromptTest, NeverTouchesTheWorkspace) {
  const agent::PromptFacts facts = facts_for(true);
  const std::string prompt = make_system_prompt(facts, "ada", "/home/ada", paths_on_path());

  EXPECT_FALSE(has(prompt, "Workspace:"));
  EXPECT_FALSE(has(prompt, "cwd:"));
  EXPECT_FALSE(has(prompt, "git repo"));
  EXPECT_FALSE(facts.workspace_cache.has_value());
}

}  // namespace
}  // namespace sp
