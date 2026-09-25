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

std::string tool_guidance_rules(const PromptFacts& facts) {
  std::ostringstream out;

  const char* shell = facts.enable_bash_repl ? "`bash_repl`" : "`bash`";

  if (facts.enable_python) {
    out << "- Use `python` for computation, file I/O, and data "
           "transformation. Use " << shell
        << " for shell commands: running programs, git, and anything the "
           "shell does more directly than Python would. Prefer one of them "
           "over describing what you would do.\n";
    if (facts.enable_package_install) {
      out << "- If a script needs a package that isn't installed, call "
             "`package_install` with just its name first, then run the "
             "script. Don't call it again for a package you already installed "
             "or that already imported successfully.\n";
    } else {
      out << "- Only the Python standard library and already-installed "
             "packages are importable; you cannot install new ones.\n";
    }
  } else {
    out << "- Use " << shell << " for all shell operations.\n";
  }

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
         << facts.tool_names.size() << " tools: "
         << join_tool_names(facts.tool_names)
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
