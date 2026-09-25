#include <core/agent.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <core/agent_pool.h>
#include <core/memory_store.h>
#include <core/system_prompt.h>
#include <core/tools_util.h>

namespace agent {

namespace {

// A tool result longer than this is clipped before it goes back to the model.
// The tools already cap themselves at 100KB, which is still far more than a
// step's worth of context is worth spending.
constexpr size_t kMaxToolResultBytes = 16000;

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

Agent::Agent(AgentOptions options, const PolicyInterface& pol, std::string id,
             std::string parent_id, int depth)
    : mOptions(std::move(options)),
      mPolicy(pol),
      mStore(mOptions.session_dir),
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
  std::vector<std::string> schemas;
  if (mOptions.enable_python) schemas.push_back(PythonTool().description());
  if (mOptions.enable_bash_repl) {
    schemas.push_back(BashReplTool::description());
  } else {
    schemas.push_back(BashTool().description());
  }
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
  if (mOptions.enable_bash_search) {
    schemas.push_back(BashSearchTool().description());
  }
  if (mOptions.enable_memory) schemas.push_back(MemoryTool::description());
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
  std::vector<std::string> names;
  if (mOptions.enable_python) names.push_back("python");
  names.push_back(mOptions.enable_bash_repl ? "bash_repl" : "bash");
  if (mOptions.enable_file_tools) {
    names.push_back("read");
    names.push_back("write");
    names.push_back("edit");
  }
  if (mOptions.enable_package_install) names.push_back("package_install");
  if (mOptions.enable_web_search) names.push_back("websearch");
  if (mOptions.enable_bash_search) names.push_back("bash_search");
  if (mOptions.enable_memory) names.push_back("memory");
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

BashReplSession& Agent::shell() {
  if (not mShell) mShell = std::make_unique<BashReplSession>();
  return *mShell;
}

void Agent::reset() {
  mTranscript.clear();
  mSessionId.clear();
  mContextTokens.store(0);
  mCatalog.reset();  // pick up skills added since the last scan
  mShell.reset();    // a new conversation gets a clean shell, not the old one
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
  const std::string summary_prompt =
      mOptions.summary_system_prompt.empty()
          ? default_summary_prompt()
          : mOptions.summary_system_prompt;
  request.push_back(ChatMessage{"system", summary_prompt, {}, ""});
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

PromptFacts Agent::prompt_facts() const {
  PromptFacts facts;
  facts.depth = mDepth;
  facts.max_depth = mOptions.max_depth;
  facts.max_agents = mOptions.max_agents;
  facts.free_agent_slots =
      std::max(0, mOptions.max_agents - AgentPool::instance().live_count());
  facts.tool_names = tool_names();

  facts.enable_python = mOptions.enable_python;
  facts.enable_bash_repl = mOptions.enable_bash_repl;
  facts.enable_package_install = mOptions.enable_package_install;
  facts.enable_file_tools = mOptions.enable_file_tools;
  facts.enable_web_search = mOptions.enable_web_search;
  facts.enable_bash_search = mOptions.enable_bash_search;
  facts.enable_memory = mOptions.enable_memory;
  facts.enable_subagents = mOptions.enable_subagents;
  facts.ask_user_offered = ask_user_offered();
  facts.can_spawn_subagents =
      mOptions.enable_subagents and mDepth < mOptions.max_depth;

  // Only when the `skill` tool is actually advertised: a catalog the model was
  // given no way to load is not a fact about its situation.
  if (skills_offered()) facts.skills = &catalog();

  return facts;
}

std::string Agent::system_prompt() const {
  const PromptFacts facts = prompt_facts();
  return mOptions.system_prompt_builder
             ? mOptions.system_prompt_builder(facts)
             : default_system_prompt(facts);
}

ToolResult Agent::dispatch(const std::string& tool_name, const ToolArgs& args) {
  if (mOptions.enable_python and tool_name == "python")
    return PythonTool().execute(args);
  if (mOptions.enable_bash_repl and tool_name == "bash_repl")
    return BashReplTool{shell()}.execute(args);
  if (not mOptions.enable_bash_repl and tool_name == "bash")
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
  if (mOptions.enable_bash_search and tool_name == "bash_search")
    return BashSearchTool().execute(args);
  if (mOptions.enable_memory and tool_name == "memory") {
    MemoryOptions memory;
    memory.path = mOptions.memory_path;
    memory.embed_model = mOptions.memory_embed_model;
    return MemoryTool{std::move(memory)}.execute(args);
  }
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

  const std::vector<std::string> schemas = tool_schemas();

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
        OllamaClient::instance().enqueue_chat(messages, schemas);
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
      ChatMessage tool_message{"tool",
                               clip_text(content, kMaxToolResultBytes),
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
