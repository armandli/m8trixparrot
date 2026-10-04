#ifndef M8_AGENT_SYSTEM_PROMPT_H
#define M8_AGENT_SYSTEM_PROMPT_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/agent/skills.h>
#include <core/agent/workspace_context.h>
#include <core/mcp/catalog.h>

namespace agent {

// What an agent's prompt needs to know about MCP this step. `catalog` is the
// turn's pinned snapshot, borrowed for the call; null means MCP is off.
struct McpFacts {
  const mcp::Catalog* catalog = nullptr;
  // The MCP schemas are deferred: only `tool_search`, the always-loaded
  // tools and whatever has been loaded are in the tools array.
  bool deferred = false;
  bool tool_search_offered = false;
  bool resources_offered = false;
  std::vector<std::string> loaded;  // exposed names loaded via tool_search
  // MCP tools in the tools array right now (all of them when not deferred).
  size_t tools_in_array = 0;
};

// Everything about an agent's situation that only the Agent knows, handed to a
// prompt builder so the builder needs no access to Agent's internals.
//
// This is the whole contract between core and an application's prompt: core
// supplies the facts, the application decides what — if anything — to say
// about them. No prompt text lives in core beyond default_system_prompt()
// below, which exists only as the fallback for an AgentOptions that names no
// builder.
struct PromptFacts {
  int depth = 0;
  int max_depth = 3;
  int max_agents = 16;
  int free_agent_slots = 0;

  // The tools advertised to the model, in the order tool_schemas() lists them.
  std::vector<std::string> tool_names;

  // `bash_repl` stands in for `bash`: one persistent shell, state included.
  bool enable_bash_repl = false;
  bool enable_file_tools = false;
  bool enable_web_search = false;
  bool enable_bash_search = false;
  bool enable_memory = false;
  bool enable_subagents = false;
  bool ask_user_offered = false;

  // Subagents are enabled *and* this agent is not already at max_depth, so a
  // subagent_create call would actually succeed.
  bool can_spawn_subagents = false;

  // Null when skills are off or the catalog holds nothing model-invocable.
  // Borrowed from the Agent, which caches it; valid for the call's duration.
  const SkillCatalog* skills = nullptr;

  McpFacts mcp;

  bool is_root() const { return depth == 0; }
  bool has_tool(std::string_view name) const;

  // Detected on the first call and cached for the rest of this PromptFacts'
  // life. Lazy because detection shells out to git: a prompt that never
  // mentions the workspace should not pay for `git status` on every single
  // model call, which is what the one built-in prompt used to cost everybody.
  const WorkspaceContext& workspace() const;

  mutable std::optional<WorkspaceContext> workspace_cache;
};

// ---------------------------------------------------------------------------
// Shared mechanics. These are formatting helpers, not prompt text — the point
// of this header is that applications do not share wording.
// ---------------------------------------------------------------------------

// "`a`, `b`, and `c`", for a prompt's tool sentence.
std::string join_tool_names(const std::vector<std::string>& names);

// "Workspace:\n- cwd: ...\n- git repo: ..." — the cwd/git block, for the
// applications whose prompt wants one. Calls facts.workspace(), so calling it
// is what triggers the git detection.
std::string workspace_block(const PromptFacts& facts, size_t status_limit = 2000);

// The "Skills available" paragraph and catalog listing, or "" when
// facts.skills is null or holds nothing model-invocable.
std::string skills_block(const PromptFacts& facts, size_t limit = 4000);

// The MCP section: each server and its tool count, how to reach the tools
// (directly, or through tool_search when deferred, with the deferred names
// grouped by server and capped at `limit`), and each server's instructions,
// labelled as the server's own words. "" when MCP is off or no server is
// configured.
std::string mcp_block(const PromptFacts& facts, size_t limit = 6000);

// "`bash`, `skill`" plus ", and N MCP tools (mcp__<server>__<tool>)" when MCP
// tools sit in the tools array undeferred — listing forty names in one
// sentence would only bury the rest.
std::string tools_sentence(const PromptFacts& facts);

// ---------------------------------------------------------------------------

// The fallback used when AgentOptions::system_prompt_builder is unset: a short
// generic role line, the tool sentence, and the two loop-contract rules.
// Deliberately minimal — every application in this repo supplies its own
// builder, and this covers a default-constructed AgentOptions, the `toolcall`
// CLI, and tests.
std::string default_system_prompt(const PromptFacts& facts);

// What each enabled tool is for, as "Working rules:" bullets: the shell, the
// persistent-shell contract, subagents, websearch, memory. This is tool
// documentation rather than application identity — a model given `bash_repl`
// needs to be told what a shell that outlives the call is for whoever is
// asking — so applications share it where the wording fits and write their own
// where it doesn't.
std::string tool_guidance_rules(const PromptFacts& facts);

// The rule pair every agent loop depends on, regardless of application: end
// the turn with a plain message, and don't retry a call that just failed.
// Applications that phrase these themselves (sp folds them into its numbered
// rules) need not use it.
std::string loop_contract_rules();

// The default system prompt for transcript compaction, used when
// AgentOptions::summary_system_prompt is empty.
std::string default_summary_prompt();

}  // namespace agent

#endif  // M8_AGENT_SYSTEM_PROMPT_H