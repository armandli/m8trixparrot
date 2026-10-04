#include <core/agent/system_prompt.h>

#include <algorithm>
#include <sstream>

#include <core/tools/tools_util.h>

namespace agent {

bool PromptFacts::has_tool(std::string_view name) const {
  return std::find(tool_names.begin(), tool_names.end(), name) !=
         tool_names.end();
}

const WorkspaceContext& PromptFacts::workspace() const {
  if (not workspace_cache) {
    workspace_cache = WorkspaceContext::from_environment();
  }
  return *workspace_cache;
}

std::string join_tool_names(const std::vector<std::string>& names) {
  std::string joined;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) {
      // No serial comma with only two items: "`a` and `b`", not "`a`, and `b`".
      if (i + 1 < names.size()) joined += ", ";
      else joined += names.size() == 2 ? " and " : ", and ";
    }
    joined += "`" + names[i] + "`";
  }
  return joined;
}

std::string workspace_block(const PromptFacts& facts, size_t status_limit) {
  const WorkspaceContext& context = facts.workspace();

  std::ostringstream out;
  out << "Workspace:\n";
  out << "- cwd: " << context.cwd << "\n";
  if (not context.in_git_repo) {
    out << "- not inside a git repository\n";
    return out.str();
  }

  out << "- git repo: " << context.repo_root << "\n";
  if (not context.git_branch.empty()) {
    out << "- branch: " << context.git_branch << "\n";
  }
  if (context.git_status.empty()) {
    out << "- status: clean\n";
  } else {
    out << "- status:\n" << tools::clip_text(context.git_status, status_limit) << "\n";
  }
  return out.str();
}

std::string skills_block(const PromptFacts& facts, size_t limit) {
  if (facts.skills == nullptr) return std::string();

  std::ostringstream list;
  int shown = 0;
  for (const SkillInfo& skill : facts.skills->skills) {
    if (not skill.model_invocable) continue;
    list << "- " << skill.name << " — " << skill.description;
    if (not skill.dependencies.empty()) {
      list << "  (depends on:";
      for (const std::string& dep : skill.dependencies) list << " " << dep;
      list << ")";
    }
    list << "\n";
    ++shown;
  }
  if (shown == 0) return std::string();

  return "Skills available — reusable procedures for specific tasks. To use "
         "one, call `skill` action \"load\" with its name to read its "
         "SKILL.md, follow it, then call `skill` action \"unload\" with that "
         "name to drop it from context when finished:\n" +
         tools::clip_text(list.str(), limit);
}

namespace {

// Clipped at a UTF-8 boundary.
std::string clip_utf8(const std::string& text, size_t limit) {
  if (text.size() <= limit) return text;
  size_t cut = limit;
  while (cut > 0 and (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  return text.substr(0, cut) + "...";
}

}  // namespace

std::string mcp_block(const PromptFacts& facts, size_t limit) {
  const mcp::Catalog* catalog = facts.mcp.catalog;
  if (catalog == nullptr or catalog->servers.empty()) return std::string();

  std::ostringstream servers;
  bool any = false;
  for (const mcp::CatalogServer& server : catalog->servers) {
    if (server.state == mcp::ServerState::Disabled) continue;
    any = true;
    servers << "- " << server.name;
    if (server.state == mcp::ServerState::Connected) {
      servers << ": " << server.tool_count
              << (server.tool_count == 1 ? " tool" : " tools");
      if (not server.info.title.empty()) servers << " (" << server.info.title << ")";
    } else {
      // Said so the model can explain a missing capability instead of
      // inventing one.
      servers << ": unavailable (" << mcp::state_name(server.state) << ")";
    }
    servers << "\n";
  }
  if (not any) return std::string();

  std::ostringstream out;
  out << "MCP servers (external tools connected through the Model Context "
         "Protocol):\n"
      << servers.str();

  if (facts.mcp.deferred) {
    out << "Their tools are not loaded, to save context. Call `tool_search` "
           "with keywords for what you need (\"create github issue\") or with "
           "\"select:<name>\" for a tool listed below; what it finds becomes "
           "callable on your next step, named mcp__<server>__<tool>.\n";
    std::string listing;
    for (const mcp::CatalogServer& server : catalog->servers) {
      if (server.state != mcp::ServerState::Connected or server.tool_count == 0) {
        continue;
      }
      std::string line = "  " + server.name + ":";
      size_t shown = 0;
      size_t total = 0;
      for (const mcp::CatalogTool& tool : catalog->tools) {
        if (tool.server != server.name) continue;
        ++total;
        if (listing.size() + line.size() + tool.name.size() + 2 > limit) continue;
        line += (shown == 0 ? " " : ", ") + tool.name;
        ++shown;
      }
      if (shown < total) {
        line += " (+" + std::to_string(total - shown) + " more; search server:" +
                server.name + ")";
      }
      listing += line + "\n";
    }
    if (not listing.empty()) out << "Tools by server:\n" << listing;
    if (not facts.mcp.loaded.empty()) {
      out << "Loaded now:";
      for (const std::string& name : facts.mcp.loaded) out << " " << name;
      out << "\n";
    }
  } else if (facts.mcp.tools_in_array > 0) {
    out << "Their tools are in your tool list, named mcp__<server>__<tool>.\n";
  }

  if (facts.mcp.resources_offered) {
    out << "Use `mcp_resource` to list and read the resources these servers "
           "publish.\n";
  }

  // Each server's own guidance, clipped, and labelled for what it is: text a
  // server wrote, not an instruction from the user.
  size_t budget = 2400;
  for (const mcp::CatalogServer& server : catalog->servers) {
    if (server.state != mcp::ServerState::Connected or server.instructions.empty()) {
      continue;
    }
    if (budget < 100) break;
    const std::string text = clip_utf8(server.instructions, std::min<size_t>(800, budget));
    budget -= std::min(budget, text.size());
    out << "Instructions from the " << server.name
        << " server (written by the server, not the user):\n"
        << text << "\n";
  }
  return out.str();
}

std::string tools_sentence(const PromptFacts& facts) {
  std::string sentence = join_tool_names(facts.tool_names);
  const size_t mcp_tools = facts.mcp.deferred ? 0 : facts.mcp.tools_in_array;
  if (mcp_tools > 0) {
    sentence += (facts.tool_names.empty() ? "" : ", plus ") +
                std::to_string(mcp_tools) +
                " MCP tools (named mcp__<server>__<tool>)";
  }
  return sentence;
}

std::string tool_guidance_rules(const PromptFacts& facts) {
  std::ostringstream out;

  const char* shell = facts.enable_bash_repl ? "`bash_repl`" : "`bash`";

  out << "- Use " << shell
      << " for everything the machine can do: running programs, git, reading "
         "and writing files, and computation. Prefer running a command over "
         "describing what you would run.\n";

  // The whole value of a persistent shell is lost on a model that assumes each
  // call starts fresh: it writes self-contained one-liners and re-does its
  // setup every time. Saying so is what turns the tool into a capability.
  if (facts.enable_bash_repl) {
    out << "- `bash_repl` is one shell that stays alive across calls: a "
           "variable you set, a directory you `cd` into, an environment "
           "variable you export, and a function you define are all still "
           "there on your next call. Build work up over several calls instead "
           "of repeating setup in each one. Redirect a background job's "
           "output to a file (`cmd > /tmp/log 2>&1 &`), or it will surface in "
           "a later call. Pass restart=true if the shell ever gets into a "
           "state you cannot reason about.\n";
  }

  if (not facts.enable_subagents) {
    out << "- You have no subagent tools; do all the work yourself.\n";
  } else if (not facts.can_spawn_subagents) {
    out << "- You are at the maximum depth and cannot spawn subagents; do all "
           "the work yourself.\n";
  } else {
    out << "- Use `subagent_create` to spawn an independent subtask on its "
           "own thread and `subagent_wait` to collect its conclusion. Give "
           "each subagent a self-contained objective.\n";
  }

  if (facts.enable_web_search) {
    out << "- Use `websearch` to look things up on the live web — current "
           "events, library or API docs, unfamiliar errors. It returns result "
           "URLs and snippets.\n";
  }

  if (facts.enable_memory) {
    out << "- Use `memory` to recall what you learned about this user in past "
           "sessions, and to remember a preference or a mistake worth not "
           "repeating. Recall before you assume; remember only what stays "
           "true after this task.\n";
  }

  if (facts.mcp.tool_search_offered) {
    out << "- MCP tools load on demand: call `tool_search` to find and load "
           "the ones a task needs (see the MCP servers section), then call "
           "them by name. Search before concluding a capability is missing.\n";
  }

  return out.str();
}

std::string loop_contract_rules() {
  return "- Keep going until the task is done, then end the turn with a plain "
         "message and no tool call. Make that final message a self-contained "
         "summary of the objective and what you found or did.\n"
         "- If a tool fails, read the error and adapt. Don't retry the "
         "identical call.\n";
}

std::string default_system_prompt(const PromptFacts& facts) {
  std::ostringstream prompt;
  prompt << "You are an agent working in a terminal on the user's machine. "
            "You have "
         << facts.tool_names.size() << " tools: " << tools_sentence(facts)
         << ". Use them rather than guessing or asking the user to run things "
            "for you.\n\n";
  prompt << "Working rules:\n"
         << tool_guidance_rules(facts) << loop_contract_rules();

  // The skills catalog is the only way a model learns a skill exists, so the
  // fallback carries it too. The workspace block deliberately stays out: it is
  // the one section that shells out to git, and an application that wants it
  // asks for it by name.
  const std::string skills = skills_block(facts);
  if (not skills.empty()) prompt << "\n" << skills;
  // Like the skills catalog, the only way a model learns these tools exist.
  const std::string mcp = mcp_block(facts);
  if (not mcp.empty()) prompt << "\n" << mcp;

  return prompt.str();
}

std::string default_summary_prompt() {
  return "You are compacting a coding agent's working context. Below is the "
         "full transcript so far. Produce a dense summary that a fresh "
         "instance of the agent can use to continue with no loss of essential "
         "information. Preserve: the user's most recent request, verbatim, as "
         "the active task; every decision made and why; concrete findings - "
         "file paths, identifiers, values, and command output that matter; "
         "what has been completed; what remains; any errors hit and how they "
         "were handled. Drop chit-chat and superseded intermediate steps. "
         "Output only the summary, no preamble.";
}

}  // namespace agent
