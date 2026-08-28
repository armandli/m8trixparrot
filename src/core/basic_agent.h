#ifndef BASIC_AGENT_H
#define BASIC_AGENT_H

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <core/ollama_client.h>
#include <core/policy.h>
#include <core/session_store.h>
#include <core/tools.h>

namespace agent {

// Everything the agent keeps lives under here, relative to the working
// directory: sessions in .m8trix/sessions, memory at kMemoryPath.
inline constexpr const char* kAgentRootDir = ".m8trix";
inline constexpr const char* kAgentSessionDir = ".m8trix/sessions";

// One thing that happened during a turn, reported as it happens rather than
// batched at the end — a turn runs several model calls and tool executions, and
// a caller that only sees the result can't show progress or explain a stall.
struct AgentEvent {
  enum struct Kind {
    Assistant,   // Model text, whether or not the turn is over.
    ToolCall,    // About to run `tool_name` with `summary`.
    ToolResult,  // `text` is what the tool produced.
    Denied,      // The policy refused; `text` is its reason.
    Error,       // The turn is failing; `text` says why.
    Notice,      // Everything else worth showing (step limit, resume, ...).
  };

  Kind kind = Kind::Assistant;
  std::string text;
  std::string tool_name;  // ToolCall / ToolResult / Denied only.
  std::string summary;    // One-line rendering of the call's key arguments.
};

using AgentObserver = std::function<void(const AgentEvent&)>;

struct AgentOptions {
  // How many model calls one user turn may take before the agent gives up. A
  // loop that keeps calling tools without answering is the failure this
  // guards against.
  int max_steps = 12;
};

struct AgentTurnResult {
  bool ok = false;
  std::string reply;  // The model's final answer.
  std::string error;
  int steps = 0;
  bool hit_step_limit = false;
};

// A minimal coding agent: the model calls tools, the tools run under a
// permission policy, and the transcript persists so a session can be resumed.
//
// One turn is one user message plus however many model/tool round trips it
// takes to answer it. run_turn() blocks for the whole thing, so a UI runs it
// off the main thread and renders from the observer callback.
//
// Forward declaration so SubagentRecord can hold a unique_ptr<BasicAgent>
// before BasicAgent is fully defined.
struct BasicAgent;

// Owns a running subagent and its result. Held via shared_ptr by both the
// parent's registry and the subagent's detached thread, so the record outlives
// the parent BasicAgent if the thread is still in flight.
//
// The destructor is defined in basic_agent.cpp so that unique_ptr<BasicAgent>
// can be destroyed there, where BasicAgent is fully defined.
struct SubagentRecord {
  std::unique_ptr<BasicAgent> agent;
  std::mutex mutex;
  std::condition_variable cv;
  std::optional<AgentTurnResult> result;  // Written by the thread when done.
  ~SubagentRecord();
};

// Tool structs for subagent_create and subagent_wait. Unlike the stateless
// tools in tools.h, these need per-agent state injected at construction time.
// description() is static (schemas are compile-time constants); execute()
// uses the injected references.
struct SubagentCreateTool {
  std::unordered_map<std::string, std::shared_ptr<SubagentRecord>>& subagents;
  std::mutex& mutex;
  const PolicyInterface& policy;
  const AgentOptions& options;

  static std::string description();
  ToolResult execute(const ToolArgs& args) const;
};

struct SubagentWaitTool {
  std::unordered_map<std::string, std::shared_ptr<SubagentRecord>>& subagents;
  std::mutex& mutex;

  static std::string description();
  ToolResult execute(const ToolArgs& args) const;
};

// Thread-safety: a single BasicAgent instance is NOT safe for concurrent
// run_turn() calls — mTranscript and mSessionId are unprotected. Concurrency
// is achieved by creating one BasicAgent per subagent thread; all agents share
// the OllamaClient singleton whose work queue serialises Ollama calls.
struct BasicAgent {
  // `policy` is borrowed and must outlive the agent.
  BasicAgent(AgentOptions options, const PolicyInterface& policy);

  AgentTurnResult run_turn(const std::string& user_input,
                           const AgentObserver& observer);

  // Loads a stored session into the transcript. An empty `session_id` takes
  // the most recently written one.
  SessionResult resume(const std::string& session_id = "");
  // Writes the transcript out. run_turn() already does this after each turn,
  // so a crash costs at most the turn in flight.
  SessionStoreResult save() const;

  const std::string& session_id() const { return mSessionId; }
  const std::vector<ChatMessage>& transcript() const { return mTranscript; }

  // Current contents of kMemoryPath; empty when the model hasn't written any.
  std::string memory() const;

  // Drops the transcript and starts a new session id. Memory survives, since
  // it is the part meant to outlive a conversation.
  void reset();

  // The tool schemas handed to the model, in the order they are advertised.
  static std::vector<std::string> tool_schemas();

protected:
  // Runs one tool call, policy first. Never throws and never reports failure
  // as anything but a ToolResult: a tool that fails is information the model
  // needs, not a reason to abandon the turn.
  ToolResult dispatch(const std::string& tool_name, const ToolArgs& args);

  // The system message, rebuilt for every model call so a memory update takes
  // effect on the very next step. Deliberately not stored in the transcript —
  // the session file holds the conversation, not generated preamble.
  std::string system_prompt() const;

private:
  AgentOptions mOptions;
  const PolicyInterface& mPolicy;
  SessionStore mStore;
  std::vector<ChatMessage> mTranscript;
  std::string mSessionId;
  mutable std::mutex mSubagentsMutex;
  std::unordered_map<std::string, std::shared_ptr<SubagentRecord>> mSubagents;
};

}  // namespace agent

#endif  // BASIC_AGENT_H
