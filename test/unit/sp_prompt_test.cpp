// sp's system prompt. sp is a shell assistant, so the tests assert both what
// the prompt says and what it no longer says: none of the coding-agent text
// the one built-in prompt used to force on every application.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent/system_prompt.h>

#include <sp_memory.h>
#include <sp_prompt.h>
#include <sp_paths.h>

namespace sp {
namespace {

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// sp's tool set with subagents off — the `--no-subagents` run.
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

// sp's default: subagents on, three deep, eight slots. `depth` picks the
// altitude, and can_spawn_subagents is computed the way Agent::prompt_facts()
// computes it so a test can't promise a call the pool would refuse.
agent::PromptFacts subagent_facts(bool memory, int depth) {
  agent::PromptFacts facts = facts_for(memory);
  facts.depth = depth;
  facts.max_depth = 3;
  facts.max_agents = 8;
  facts.free_agent_slots = 8;
  facts.enable_subagents = true;
  facts.can_spawn_subagents = depth < facts.max_depth;
  facts.tool_names.push_back("subagent_create");
  facts.tool_names.push_back("subagent_wait");
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

std::string root_prompt(bool memory) {
  return make_system_prompt(subagent_facts(memory, 0), "ada", "/home/ada",
                            paths_on_path());
}

std::string worker_prompt(bool memory, int depth = 1) {
  return make_system_prompt(subagent_facts(memory, depth), "ada", "/home/ada",
                            paths_on_path());
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

// `facts_for` is the --no-subagents run: an agent that was not given the tools
// must not be told a word about them.
TEST(SpPromptTest, CarriesNoAgentTreeOrSubagentText) {
  const std::string prompt = prompt_for(true);
  EXPECT_FALSE(has(prompt, "agent slots are free"));
  EXPECT_FALSE(has(prompt, "subagent"));
  EXPECT_FALSE(has(prompt, "depth"));
}

// ──────────────────────────── delegating ───────────────────────────────────

TEST(SpPromptTest, RootIsToldWhenToDelegateAndWhenNotTo) {
  const std::string prompt = root_prompt(false);
  EXPECT_TRUE(has(prompt, "Delegating work:"));
  EXPECT_TRUE(has(prompt, "8 of 8 agent slots are free"));
  EXPECT_TRUE(has(prompt, "depth 0 of max 3"));
  EXPECT_TRUE(has(prompt, "3 more level(s) below you"));
  EXPECT_TRUE(has(prompt, "Do NOT delegate a single command"));
  EXPECT_TRUE(has(prompt, "`subagent_create`"));
  EXPECT_TRUE(has(prompt, "`subagent_wait`"));
}

// The stall this prevents is invisible from inside the model's loop: the root
// can finish speaking while save()/assemble_tree still waits on the child.
TEST(SpPromptTest, RootMustWaitForEverySubagentItCreates) {
  const std::string prompt = root_prompt(false);
  EXPECT_TRUE(has(prompt, "ALWAYS `subagent_wait` on every id you created"));
  EXPECT_TRUE(has(prompt, "holds the turn open"));
}

// A subagent's shell starts where sp was launched, not where its caller stood,
// and none of the caller's variables exist in it.
TEST(SpPromptTest, RootIsToldObjectivesCarryAbsolutePaths) {
  const std::string prompt = root_prompt(false);
  EXPECT_TRUE(has(prompt, "spell out ABSOLUTE paths"));
  EXPECT_TRUE(has(prompt, "the directory we were just in"));
  EXPECT_TRUE(has(prompt, "shares nothing with you"));
}

TEST(SpPromptTest, PutsDelegatingAfterTheShellRulesAndBeforeMemory) {
  const std::string prompt = root_prompt(true);
  EXPECT_LT(prompt.find("Rules:"), prompt.find("Delegating work:"));
  EXPECT_LT(prompt.find("Delegating work:"), prompt.find("Memory:"));
}

// A subagent that can still nest gets the section too — that is the recursion.
TEST(SpPromptTest, ASubagentThatCanStillNestIsToldHowToDelegate) {
  const std::string prompt = worker_prompt(false, 1);
  EXPECT_TRUE(has(prompt, "Delegating work:"));
  EXPECT_TRUE(has(prompt, "depth 1 of max 3"));
  EXPECT_TRUE(has(prompt, "2 more level(s) below you"));
}

// can_spawn_subagents is false at the floor, so nothing promises a call that
// AgentPool::spawn would refuse.
TEST(SpPromptTest, SaysNothingAboutDelegatingAtMaxDepth) {
  const std::string prompt = worker_prompt(false, 3);
  EXPECT_FALSE(has(prompt, "Delegating work:"));
  EXPECT_FALSE(has(prompt, "agent slots are free"));
  // It is still a subagent, and still knows it.
  EXPECT_TRUE(has(prompt, "subagent at depth 3 of max 3"));
}

// ──────────────────────── the subagent's own prompt ─────────────────────────

TEST(SpPromptTest, ASubagentKnowsOnlyItsFinalMessageIsRead) {
  const std::string prompt = worker_prompt(false);
  EXPECT_TRUE(has(prompt, "You are a shell-parrot subagent at depth 1"));
  EXPECT_TRUE(has(prompt, "Your caller sees ONLY your final message"));
  EXPECT_TRUE(has(prompt, "ABSOLUTE path of every file"));
  EXPECT_TRUE(has(prompt, "has thrown the whole task away"));
  // One role definition, not the root's as well.
  EXPECT_FALSE(has(prompt, "You are shell-parrot (sp), a natural-language"));
}

TEST(SpPromptTest, ASubagentIsNotToldToInstallScripts) {
  const std::string prompt = worker_prompt(false);
  EXPECT_FALSE(has(prompt, "Writing a reusable script:"));
  EXPECT_FALSE(has(prompt, "Telling the user about a script:"));
  EXPECT_FALSE(has(prompt, "chmod +x"));
  EXPECT_FALSE(has(prompt, "<<'EOF'"));
  EXPECT_TRUE(has(prompt, "Do NOT install commands"));
}

// Everything factual about the machine survives the altitude change.
TEST(SpPromptTest, ASubagentKeepsTheTrashRuleAndGetsItsOwnShell) {
  const std::string prompt = worker_prompt(false);
  EXPECT_TRUE(has(prompt, "NEVER use rm"));
  EXPECT_TRUE(has(prompt, "/home/ada/.local/share/sp/trash"));
  EXPECT_TRUE(has(prompt, "ONE shell that stays alive"));
  EXPECT_TRUE(has(prompt, "Your shell is yours alone"));
  EXPECT_TRUE(has(prompt, "NOT wherever your caller had `cd`'d to"));
}

// Concurrent writers cannot keep "one memory per subject" — none of them can
// see what the others just stored.
TEST(SpPromptTest, ASubagentRecallsMemoryButNeverWritesIt) {
  const std::string prompt = worker_prompt(true);
  EXPECT_TRUE(has(prompt, "action='recall'"));
  EXPECT_TRUE(has(prompt, "NEVER call action='remember'"));
  EXPECT_TRUE(has(prompt, "starting with MEMORY:"));
  // The root's writing rules are the root's alone.
  EXPECT_FALSE(has(prompt, "PREFERENCE (<subject>):"));
  EXPECT_FALSE(has(prompt, "type='semantic'"));
  EXPECT_FALSE(has(prompt, "Never use type='episodic'"));
}

// A MEMORY: line from depth 2 would otherwise die at depth 1.
TEST(SpPromptTest, AMiddleSubagentForwardsWhatItsChildrenReport) {
  const std::string prompt = worker_prompt(true, 1);
  EXPECT_TRUE(has(prompt, "pass it up"));

  // The floor has no children to forward for.
  const std::string floor_prompt = worker_prompt(true, 3);
  EXPECT_FALSE(has(floor_prompt, "pass it up"));
}

TEST(SpPromptTest, RootStoresWhatItsSubagentsReport) {
  const std::string prompt = root_prompt(true);
  EXPECT_TRUE(has(prompt, "Your subagents are not allowed to store memories"));
  EXPECT_TRUE(has(prompt, "line starting MEMORY:"));
  // Nothing to harvest when there are no subagents.
  EXPECT_FALSE(has(prompt_for(true), "MEMORY:"));
}

TEST(SpPromptTest, ASubagentWithMemoryOffIsToldNothingAboutIt) {
  const std::string prompt = worker_prompt(false);
  EXPECT_FALSE(has(prompt, "action='recall'"));
  EXPECT_FALSE(has(prompt, "MEMORY:"));
}

// The lazy-workspace guarantee has to hold at every depth, not just the root.
TEST(SpPromptTest, ASubagentNeverTouchesTheWorkspaceEither) {
  const agent::PromptFacts facts = subagent_facts(true, 1);
  const std::string prompt =
      make_system_prompt(facts, "ada", "/home/ada", paths_on_path());

  EXPECT_FALSE(has(prompt, "Workspace:"));
  EXPECT_FALSE(has(prompt, "git repo"));
  EXPECT_FALSE(facts.workspace_cache.has_value());
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
