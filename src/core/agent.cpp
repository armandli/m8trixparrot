#include <core/agent.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <core/agent_pool.h>
#include <core/tools_util.h>
#include <core/workspace_context.h>

namespace agent {

namespace {

// A tool result longer than this is clipped before it goes back to the model.
// The tools already cap themselves at 100KB, which is still far more than a
// step's worth of context is worth spending.
constexpr size_t kMaxToolResultBytes = 16000;

std::string clip(const std::string& text, size_t limit) {
  if (text.size() <= limit) return text;
  return text.substr(0, limit) + "\n[... clipped, " +
         std::to_string(text.size() - limit) + " more bytes]";
}

// A one-line rendering of the arguments that matter for display, so the UI can
// show "grep pattern=\"teh\"" rather than the whole JSON object.
std::string summarize(const std::string& tool_name, const ToolArgs& args) {
  static const char* kInteresting[] = {"command",   "path",   "pattern",
                                       "query",     "url",    "content",
                                       "objective", "id",     "action",
                                       "name",      "prompt"};

  std::string summary;
  for (const char* key : kInteresting) {
    const std::optional<std::string> value = string_arg(args, key);
    if (not value) continue;
    if (not summary.empty()) summary += "  ";
    summary += std::string(key) + "=";
    // Newlines would break the one-line promise; a long value is elided.
    std::string shown = value->substr(0, 60);
    std::replace(shown.begin(), shown.end(), '\n', ' ');
    summary += "\"" + shown + (value->size() > 60 ? "..." : "") + "\"";
  }

  if (summary.empty()) return tool_name;
  return summary;
}

// "`a`, `b`, and `c`" for the system prompt's tool sentence.
std::string join_tool_names(const std::vector<std::string>& names) {
  std::string joined;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) joined += (i + 1 == names.size()) ? ", and " : ", ";
    joined += "`" + names[i] + "`";
  }
  return joined;
}

constexpr const char* kSummarySystemPrompt =
    "You are compacting a coding agent's working context. Below is the full "
    "transcript so far. Produce a dense summary that a fresh instance of the "
    "agent can use to continue with no loss of essential information. Preserve: "
    "the user's most recent request, verbatim, as the active task; every "
    "decision made and why; concrete findings - file paths, identifiers, "
    "values, and command output that matter; what has been completed; what "
    "remains; any errors hit and how they were handled. Drop chit-chat and "
    "superseded intermediate steps. Output only the summary, no preamble.";

// The transcript flattened to labelled text, for the summarizer to read. A
// message that is loaded skill content is reduced to a placeholder — the model
// can reload the skill after summarizing, so its body needn't be re-digested.
std::string render_transcript(const std::vector<ChatMessage>& transcript) {
  std::ostringstream out;
  for (const ChatMessage& message : transcript) {
    if (not message.skill_label.empty()) {
      out << "[skill '" << message.skill_label
          << "' content was loaded here]\n\n";
      continue;
    }
    out << "[" << message.role;
    if (not message.tool_name.empty()) out << " " << message.tool_name;
    out << "]\n";
    if (not message.content.empty()) out << message.content << "\n";
    for (const ToolCall& call : message.tool_calls) {
      out << "-> called " << call.name << "(" << call.arguments << ")\n";
    }
    out << "\n";
  }
  return out.str();
}

// "128k" / "1.5M" / "640".
std::string human_tokens(int64_t n) {
  if (n < 1000) return std::to_string(n);
  if (n < 1000000) return std::to_string((n + 500) / 1000) + "k";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1000000.0);
  return std::string(buf);
}

}  // namespace

Agent::Agent(AgentOptions options, const PolicyInterface& policy, std::string id,
             std::string parent_id, int depth)
    : mOptions(std::move(options)),
      mPolicy(policy),
      mStore(kAgentSessionDir),
      mId(std::move(id)),
      mParentId(std::move(parent_id)),
      mDepth(depth),
      mLabel(depth == 0 ? "root" : "subagent") {}

bool Agent::skills_offered() const {
  return mOptions.enable_skills and not catalog().skills.empty();
}

bool Agent::ask_user_offered() const {
  return static_cast<bool>(mOptions.ask_user_handler);
}

std::vector<std::string> Agent::tool_schemas() const {
  std::vector<std::string> schemas{PythonTool().description(),
                                   BashTool().description()};
  if (mOptions.enable_file_tools) {
    schemas.push_back(ReadTool().description());
    schemas.push_back(WriteTool().description());
    schemas.push_back(EditTool().description());
  }
  if (mOptions.enable_package_install) {
    schemas.push_back(PackageInstallTool().description());
  }
  if (mOptions.enable_web_search) {
    schemas.push_back(WebSearchTool().description());
  }
  if (skills_offered()) schemas.push_back(SkillTool::description());
  if (mOptions.enable_subagents) {
    schemas.push_back(SubagentCreateTool::description());
    schemas.push_back(SubagentWaitTool::description());
  }
  if (ask_user_offered()) {
    schemas.push_back(AskUserTool{mOptions.ask_user_handler}.description());
  }
  return schemas;
}

std::vector<std::string> Agent::tool_names() const {
  std::vector<std::string> names{"python", "bash"};
  if (mOptions.enable_file_tools) {
    names.push_back("read");
    names.push_back("write");
    names.push_back("edit");
  }
  if (mOptions.enable_package_install) names.push_back("package_install");
  if (mOptions.enable_web_search) names.push_back("websearch");
  if (skills_offered()) names.push_back("skill");
  if (mOptions.enable_subagents) {
    names.push_back("subagent_create");
    names.push_back("subagent_wait");
  }
  if (ask_user_offered()) names.push_back("ask_user");
  return names;
}

const SkillCatalog& Agent::catalog() const {
  if (not mCatalog) mCatalog = SkillCatalog::discover(mOptions.skills_dir);
  return *mCatalog;
}

const SkillCatalog& Agent::skill_catalog() const { return catalog(); }

void Agent::reload_skills() {
  mCatalog = SkillCatalog::discover(mOptions.skills_dir);
}

std::string Agent::skill_label_for(const ToolCall& call, const ToolArgs& args,
                                   const ToolResult& result) const {
  if (not mOptions.enable_skills) return std::string();
  if (call.name == "skill") {
    if (not result.ok) return std::string();
    const std::optional<std::string> action = string_arg(args, "action");
    const std::optional<std::string> name = string_arg(args, "name");
    if (action and name and *action == "load") return *name;
    return std::string();
  }
  if (call.name == "python") return catalog().label_for_text(call.arguments);
  return std::string();
}

void Agent::reset() {
  mTranscript.clear();
  mSessionId.clear();
  mContextTokens.store(0);
  mCatalog.reset();  // pick up skills added since the last scan
}

int64_t Agent::summarize_threshold() const {
  const int64_t hard = mOptions.context_summarize_at_tokens;
  if (mOptions.context_window_tokens <= 0) return hard;
  const int64_t soft =
      static_cast<int64_t>(mOptions.context_window_tokens) * 4 / 5;
  return std::min(hard, soft);
}

int64_t Agent::context_limit() const { return summarize_threshold(); }

int64_t Agent::context_tokens() const { return mContextTokens.load(); }

void Agent::emit_context_usage() const {
  AgentEvent event;
  event.kind = AgentEvent::Kind::ContextUsage;
  event.tokens = mContextTokens.load();
  event.token_budget = summarize_threshold();
  emit(event);
}

void Agent::maybe_summarize_context() {
  if (mTranscript.size() < 2) return;

  const int64_t current =
      std::max(mContextTokens.load(), estimate_transcript_tokens(mTranscript));
  if (current < summarize_threshold()) return;

  emit({AgentEvent::Kind::Notice,
        "context at ~" + human_tokens(current) +
            " tokens; summarizing before continuing",
        "", ""});

  // Skills loaded before the summary lose their body (render_transcript drops
  // it); tell the model which they were so it can reload any it still needs.
  std::vector<std::string> loaded;
  for (const ChatMessage& message : mTranscript) {
    if (not message.skill_label.empty() and
        std::find(loaded.begin(), loaded.end(), message.skill_label) ==
            loaded.end()) {
      loaded.push_back(message.skill_label);
    }
  }

  std::vector<ChatMessage> request;
  request.push_back(ChatMessage{"system", kSummarySystemPrompt, {}, ""});
  request.push_back(ChatMessage{"user", render_transcript(mTranscript), {}, ""});

  const uint64_t ticket = OllamaClient::instance().enqueue_chat(request, {});
  const ChatResult reply = OllamaClient::instance().wait_for(ticket);

  if (not reply.ok or reply.content.empty()) {
    emit({AgentEvent::Kind::Notice,
          "context summarization failed (" +
              (reply.error.empty() ? std::string("empty response")
                                   : reply.error) +
              "); continuing",
          "", ""});
    return;
  }

  std::string seed =
      "The earlier conversation was summarized to save context. Summary:\n\n" +
      reply.content + "\n\nContinue the task from here.";
  if (not loaded.empty()) {
    seed += "\n\nSkills loaded before this summary:";
    for (const std::string& name : loaded) seed += " " + name;
    seed += ". Reload any you still need with the `skill` tool.";
  }

  mTranscript.clear();
  mTranscript.push_back(ChatMessage{"user", seed, {}, ""});
  mContextTokens.store(estimate_transcript_tokens(mTranscript));

  AgentEvent summarized;
  summarized.kind = AgentEvent::Kind::ContextSummarized;
  summarized.text = "context summarized (~" + human_tokens(current) +
                    " tokens folded into a summary)";
  summarized.tokens = current;
  emit(summarized);
}

std::string Agent::system_prompt() const {
  const WorkspaceContext context = WorkspaceContext::from_environment();
  const int live = AgentPool::instance().live_count();
  const int free_slots = std::max(0, mOptions.max_agents - live);

  const std::vector<std::string> names = tool_names();

  std::ostringstream prompt;
  prompt << "You are a coding agent working in a terminal on the user's "
            "machine. You have "
         << names.size() << " tools: " << join_tool_names(names)
         << ". Use them rather than guessing or asking the user to run things "
            "for you.\n\n";

  if (mDepth == 0) {
    prompt << "You are the root agent (depth 0 of max " << mOptions.max_depth
           << "). ";
  } else {
    prompt << "You are a subagent at depth " << mDepth << " of max "
           << mOptions.max_depth
           << ". Your caller sees only your final message, not your steps. ";
  }
  prompt << free_slots << " of " << mOptions.max_agents
         << " agent slots are free.\n\n";

  prompt << "Working rules:\n"
            "- Use `python` for computation, file I/O, and data transformation. "
            "Use `bash` for shell commands: running programs, git, and anything "
            "the shell does more directly than Python would. Prefer one of them "
            "over describing what you would do.\n";
  if (mOptions.enable_package_install) {
    prompt << "- If a script needs a package that isn't installed, call "
              "`package_install` with just its name first, then run the "
              "script. Don't call it again for a package you already "
              "installed or that already imported successfully.\n";
  } else {
    prompt << "- Only the Python standard library and already-installed "
              "packages are importable; you cannot install new ones.\n";
  }
  if (not mOptions.enable_subagents) {
    prompt << "- You have no subagent tools; do all the work yourself.\n";
  } else if (mDepth >= mOptions.max_depth) {
    prompt << "- You are at the maximum depth and cannot spawn subagents; do "
              "all the work yourself.\n";
  } else {
    prompt << "- Use `subagent_create` to spawn an independent subtask on its "
              "own thread and `subagent_wait` to collect its conclusion. Give "
              "each subagent a self-contained objective.\n";
  }
  if (mOptions.enable_web_search) {
    prompt << "- Use `websearch` to look things up on the live web — current "
              "events, library or API docs, unfamiliar errors. It returns "
              "result URLs and snippets.\n";
  }
  prompt << "- Keep going until the task is done, then end the turn with a "
            "plain message and no tool call. Make that final message a "
            "self-contained summary of the objective and what you found or "
            "did.\n"
            "- If a tool fails, read the error and adapt. Don't retry the "
            "identical call.\n\n";

  prompt << "Workspace:\n";
  prompt << "- cwd: " << context.cwd << "\n";
  if (context.in_git_repo) {
    prompt << "- git repo: " << context.repo_root << "\n";
    if (not context.git_branch.empty()) {
      prompt << "- branch: " << context.git_branch << "\n";
    }
    if (not context.git_status.empty()) {
      prompt << "- status:\n" << clip(context.git_status, 2000) << "\n";
    } else {
      prompt << "- status: clean\n";
    }
  } else {
    prompt << "- not inside a git repository\n";
  }

  if (mOptions.enable_skills) {
    std::ostringstream list;
    int shown = 0;
    for (const SkillInfo& skill : catalog().skills) {
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
    if (shown > 0) {
      prompt << "\nSkills available — reusable procedures for specific tasks. "
                "To use one, call `skill` action \"load\" with its name to read "
                "its SKILL.md, follow it (reading its other files with `python` "
                "as needed), then call `skill` action \"unload\" with that name "
                "to drop it from context when finished:\n"
             << clip(list.str(), 4000);
    }
  }

  if (not mOptions.extra_system_prompt.empty()) {
    prompt << "\n\n" << mOptions.extra_system_prompt << "\n";
  }

  return prompt.str();
}

ToolResult Agent::dispatch(const std::string& tool_name, const ToolArgs& args) {
  if (tool_name == "python")
    return PythonTool().execute(args);
  if (tool_name == "bash")
    return BashTool().execute(args);
  if (mOptions.enable_file_tools and tool_name == "read")
    return ReadTool().execute(args);
  if (mOptions.enable_file_tools and tool_name == "write")
    return WriteTool().execute(args);
  if (mOptions.enable_file_tools and tool_name == "edit")
    return EditTool().execute(args);
  if (mOptions.enable_package_install and tool_name == "package_install")
    return PackageInstallTool().execute(args);
  if (mOptions.enable_web_search and tool_name == "websearch")
    return WebSearchTool().execute(args);
  if (ask_user_offered() and tool_name == "ask_user")
    return AskUserTool{mOptions.ask_user_handler}.execute(args);
  if (mOptions.enable_skills and tool_name == "skill")
    return SkillTool{mTranscript, mContextTokens, catalog()}.execute(args);
  if (mOptions.enable_subagents and tool_name == "subagent_create")
    return SubagentCreateTool{mId, mPolicy, mOptions}.execute(args);
  if (mOptions.enable_subagents and tool_name == "subagent_wait")
    return SubagentWaitTool{}.execute(args);

  ToolResult unknown;
  unknown.error = "no tool named '" + tool_name +
                  "' exists; call one of the tools you were given";
  return unknown;
}

SessionResult Agent::resume(const std::string& session_id) {
  // Sessions hold only the result tree, so there is no transcript to restore
  // and mSessionId stays empty — a continued conversation opens a new file.
  return session_id.empty() ? mStore.latest() : mStore.load(session_id);
}

SessionStoreResult Agent::save() const {
  if (mDepth != 0) {
    SessionStoreResult skipped;
    skipped.ok = true;
    return skipped;
  }
  return mStore.store(AgentPool::instance().assemble_tree(mId), mSessionId);
}

void Agent::emit(AgentEvent event) const {
  event.agent_id = mId;
  event.parent_id = mParentId;
  event.depth = mDepth;
  event.agent_label = mLabel;
  AgentPool::instance().emit(event);
}

AgentResult Agent::run_turn(const std::string& objective) {
  AgentResult self;
  self.objective = objective;

  mTranscript.push_back(ChatMessage{"user", objective, {}, ""});

  const std::vector<std::string> tools = tool_schemas();

  for (int step = 0; step < mOptions.max_steps; ++step) {
    self.steps = step + 1;

    // Compact the transcript before it can overflow the context window. This
    // may itself run an Ollama call and replace mTranscript with a summary.
    maybe_summarize_context();

    // The system message is rebuilt every step rather than stored, so a skill
    // load/unload lands in the very next call.
    std::vector<ChatMessage> messages;
    messages.reserve(mTranscript.size() + 1);
    messages.push_back(ChatMessage{"system", system_prompt(), {}, ""});
    messages.insert(messages.end(), mTranscript.begin(), mTranscript.end());

    const uint64_t ticket =
        OllamaClient::instance().enqueue_chat(messages, tools);
    const ChatResult reply = OllamaClient::instance().wait_for(ticket);
    if (not reply.ok) {
      self.ok = false;
      self.error = reply.error;
      emit({AgentEvent::Kind::Error, reply.error, "", ""});
      AgentPool::instance().set_result(mId, self);
      if (mDepth == 0) save();
      return self;
    }

    mContextTokens.store(reply.prompt_eval_count);
    emit_context_usage();

    mTranscript.push_back(
        ChatMessage{"assistant", reply.content, reply.tool_calls, ""});

    if (not reply.content.empty()) {
      emit({AgentEvent::Kind::Assistant, reply.content, "", ""});
    }

    // No tool calls means the model is answering, which ends the turn.
    if (reply.tool_calls.empty()) {
      self.ok = true;
      self.conclusion = reply.content;
      AgentPool::instance().set_result(mId, self);
      if (mDepth == 0) {
        const SessionStoreResult saved = save();
        if (not saved.ok) {
          emit({AgentEvent::Kind::Notice,
                "failed to save session: " + saved.error, "", ""});
        } else {
          mSessionId = saved.session_id;
        }
      }
      return self;
    }

    for (const ToolCall& call : reply.tool_calls) {
      std::string parse_error;
      const ToolArgs args = args_from_json(call.arguments, parse_error);
      const std::string summary = summarize(call.name, args);

      emit({AgentEvent::Kind::ToolCall, "", call.name, summary});

      if (not parse_error.empty()) {
        const std::string message =
            "could not read the arguments for '" + call.name + "': " +
            parse_error;
        emit({AgentEvent::Kind::ToolResult, message, call.name, ""});
        mTranscript.push_back(ChatMessage{"tool", message, {}, call.name});
        continue;
      }

      const PolicyResult verdict = mPolicy.verify(call.name, args);
      if (not verdict.allowed()) {
        // The refusal goes back as the tool's result: the model is told why and
        // can pick another approach, which is the whole point of making
        // policies explain themselves.
        emit({AgentEvent::Kind::Denied, verdict.reason, call.name, summary});
        mTranscript.push_back(
            ChatMessage{"tool", verdict.reason, {}, call.name});
        continue;
      }

      const ToolResult executed = dispatch(call.name, args);
      std::string content = executed.ok ? executed.output : executed.error;
      // A tool can legitimately produce nothing (ls of an empty directory,
      // grep with no hits). Saying so beats sending an empty message.
      if (content.empty()) content = "[no output]";

      emit({AgentEvent::Kind::ToolResult, content, call.name, ""});
      ChatMessage tool_message{"tool", clip(content, kMaxToolResultBytes),
                               {}, call.name};
      tool_message.skill_label = skill_label_for(call, args, executed);
      mTranscript.push_back(std::move(tool_message));
    }
  }

  self.ok = false;
  self.hit_step_limit = true;
  self.error = "gave up after " + std::to_string(mOptions.max_steps) +
               " steps without a final answer";
  emit({AgentEvent::Kind::Notice, self.error, "", ""});
  AgentPool::instance().set_result(mId, self);
  if (mDepth == 0) {
    const SessionStoreResult saved = save();
    if (saved.ok) mSessionId = saved.session_id;
  }
  return self;
}

}  // namespace agent
