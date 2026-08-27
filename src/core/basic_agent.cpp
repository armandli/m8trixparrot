#include <core/basic_agent.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
  static const char* kInteresting[] = {"command", "path", "pattern",
                                       "query",   "url",  "content"};

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

}  // namespace

BasicAgent::BasicAgent(AgentOptions options, const OllamaClient& client,
                       const PolicyInterface& policy)
    : mOptions(std::move(options)),
      mClient(client),
      mPolicy(policy),
      mStore(kAgentSessionDir) {}

std::vector<std::string> BasicAgent::tool_schemas() {
  return {
      BashTool().description(),   ReadTool().description(),
      WriteTool().description(),  EditTool().description(),
      FindTool().description(),   GrepTool().description(),
      MemoryTool().description(), PythonTool().description(),
  };
}

std::string BasicAgent::memory() const {
  const std::optional<std::string> contents = read_file(kMemoryPath);
  return contents.value_or(std::string());
}

void BasicAgent::reset() {
  mTranscript.clear();
  mSessionId.clear();
}

std::string BasicAgent::system_prompt() const {
  const WorkspaceContext context = WorkspaceContext::from_environment();

  std::ostringstream prompt;
  prompt << "You are a coding agent working in a terminal on the user's "
            "machine. You have tools for running shell commands and for "
            "reading, searching and editing files. Use them rather than "
            "guessing or asking the user to run things for you.\n\n"
            "Working rules:\n"
            "- Read a file before editing it; `edit` matches exact text and "
            "fails if it isn't unique.\n"
            "- Prefer `find`/`grep` over `bash` for locating things; they "
            "respect .gitignore.\n"
            "- Keep going until the task is done, then answer without calling "
            "a tool. A reply with no tool call ends the turn.\n"
            "- Call `memory` when you learn something worth carrying across "
            "turns; it replaces your notes wholesale.\n"
            "- If a tool fails or is refused, read why and adapt. Don't retry "
            "the identical call.\n\n";

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

  const std::string notes = memory();
  if (not notes.empty()) {
    prompt << "\nYour memory notes:\n" << clip(notes, 8000) << "\n";
  }

  return prompt.str();
}

ToolResult BasicAgent::dispatch(const std::string& tool_name,
                                 const ToolArgs& args) const {
  if (tool_name == "bash") return BashTool().execute(args);
  if (tool_name == "read") return ReadTool().execute(args);
  if (tool_name == "write") return WriteTool().execute(args);
  if (tool_name == "edit") return EditTool().execute(args);
  if (tool_name == "find") return FindTool().execute(args);
  if (tool_name == "grep") return GrepTool().execute(args);
  if (tool_name == "memory") return MemoryTool().execute(args);
  if (tool_name == "python") return PythonTool().execute(args);

  ToolResult unknown;
  unknown.error = "no tool named '" + tool_name +
                  "' exists; call one of the tools you were given";
  return unknown;
}

SessionResult BasicAgent::resume(const std::string& session_id) {
  SessionResult result =
      session_id.empty() ? mStore.latest() : mStore.load(session_id);
  if (not result.ok) return result;

  mTranscript = result.session.interactions;
  mSessionId = result.session.session_id;
  return result;
}

SessionStoreResult BasicAgent::save() const {
  SessionStoreResult result = mStore.store(mTranscript, mSessionId);
  return result;
}

AgentTurnResult BasicAgent::run_turn(const std::string& user_input,
                                      const AgentObserver& observer) {
  AgentTurnResult turn;

  const auto emit = [&observer](AgentEvent event) {
    if (observer) observer(event);
  };

  mTranscript.push_back(ChatMessage{"user", user_input, {}, ""});

  const std::vector<std::string> tools = tool_schemas();

  for (int step = 0; step < mOptions.max_steps; ++step) {
    turn.steps = step + 1;

    // The system message is rebuilt every step rather than stored, so a
    // memory write lands in the very next call.
    std::vector<ChatMessage> messages;
    messages.reserve(mTranscript.size() + 1);
    messages.push_back(ChatMessage{"system", system_prompt(), {}, ""});
    messages.insert(messages.end(), mTranscript.begin(), mTranscript.end());

    const ChatResult reply = mClient.chat(mOptions.model, messages, tools);
    if (not reply.ok) {
      turn.error = reply.error;
      emit({AgentEvent::Kind::Error, reply.error, "", ""});
      return turn;
    }

    mTranscript.push_back(
        ChatMessage{"assistant", reply.content, reply.tool_calls, ""});

    if (not reply.content.empty()) {
      emit({AgentEvent::Kind::Assistant, reply.content, "", ""});
    }

    // No tool calls means the model is answering, which ends the turn.
    if (reply.tool_calls.empty()) {
      turn.ok = true;
      turn.reply = reply.content;
      const SessionStoreResult saved = save();
      if (not saved.ok) {
        emit({AgentEvent::Kind::Notice,
              "failed to save session: " + saved.error, "", ""});
      } else {
        mSessionId = saved.session_id;
      }
      return turn;
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
        // The refusal goes back as the tool's result: the model is told why
        // and can pick another approach, which is the whole point of making
        // policies explain themselves.
        emit({AgentEvent::Kind::Denied, verdict.reason, call.name, summary});
        mTranscript.push_back(
            ChatMessage{"tool", verdict.reason, {}, call.name});
        continue;
      }

      const ToolResult executed = dispatch(call.name, args);
      std::string content = executed.ok ? executed.output : executed.error;
      // A tool can legitimately produce nothing (ls of an empty directory,
      // grep with no hits). Saying so beats sending an empty message, which
      // reads as a malformed turn rather than a result.
      if (content.empty()) content = "[no output]";

      emit({AgentEvent::Kind::ToolResult, content, call.name, ""});
      mTranscript.push_back(ChatMessage{
          "tool", clip(content, kMaxToolResultBytes), {}, call.name});
    }
  }

  turn.hit_step_limit = true;
  turn.error = "gave up after " + std::to_string(mOptions.max_steps) +
               " steps without a final answer";
  emit({AgentEvent::Kind::Notice, turn.error, "", ""});

  const SessionStoreResult saved = save();
  if (saved.ok) mSessionId = saved.session_id;
  return turn;
}

}  // namespace agent
