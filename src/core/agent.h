#ifndef AGENT_H
#define AGENT_H

#include <functional>
#include <string>
#include <vector>

#include <core/agent_result.h>
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
//
// Every agent in the tree, root and subagent alike, emits through the one
// process-wide observer on AgentPool, so each event carries the id, parent, and
// depth of the agent that raised it: a UI can then render the recursion as a
// tree rather than a flat stream.
struct AgentEvent {
  enum struct Kind {
    Assistant,      // Model text, whether or not the turn is over.
    ToolCall,       // About to run `tool_name` with `summary`.
    ToolResult,     // `text` is what the tool produced.
    Denied,         // The policy refused; `text` is its reason.
    Error,          // The turn is failing; `text` says why.
    Notice,         // Everything else worth showing (step limit, resume, ...).
    SubagentStart,  // A child agent began: `agent_id` is the child, `summary` its objective.
    SubagentDone,   // A child agent finished: `text` is its conclusion or error.
  };

  Kind kind = Kind::Assistant;
  std::string text;
  std::string tool_name;  // ToolCall / ToolResult / Denied only.
  std::string summary;    // One-line rendering of the call's key arguments.

  // Which agent in the tree raised this. Stamped by Agent::emit(), except
  // SubagentStart which SubagentCreateTool stamps for the child.
  std::string agent_id;
  std::string parent_id;
  int depth = 0;
  std::string agent_label;  // "root" | "subagent"
  bool ok = true;           // SubagentDone: did the child succeed.
};

using AgentObserver = std::function<void(const AgentEvent&)>;

struct AgentOptions {
  // How many model calls one turn may take before the agent gives up. A loop
  // that keeps calling tools without answering is the failure this guards
  // against.
  int max_steps = 12;

  // How deep the subagent recursion may go. The root is depth 0; a subagent
  // spawned by a depth-d agent is depth d+1. subagent_create refuses past this.
  int max_depth = 3;

  // How many agents may be live at once across the whole tree. subagent_create
  // refuses when this many are already running.
  int max_agents = 16;
};

// Forward declaration: the subagent tools reach the pool through
// AgentPool::instance() (declared in agent_pool.h), which agent.cpp includes.

// Tool structs for subagent_create and subagent_wait. Unlike the stateless
// tools in tools.h these carry the spawning agent's identity, so an aggregate
// is built at the dispatch site with the current agent's fields.
struct SubagentCreateTool {
  std::string parent_id;
  const PolicyInterface& policy;
  const AgentOptions& options;

  static std::string description();
  ToolResult execute(const ToolArgs& args) const;
};

struct SubagentWaitTool {
  static std::string description();
  ToolResult execute(const ToolArgs& args) const;
};

// A coding agent: the model calls tools, the tools run under a permission
// policy, and the agent can recursively spawn subagents that each run their own
// turn with their own chat history. Every agent is the same type; the root is
// just the one at depth 0 that persists the result tree.
//
// One turn is one task string plus however many model/tool round trips it takes
// to answer it. run_turn() blocks for the whole thing, so a UI runs it off the
// main thread and renders from the AgentPool observer.
//
// Thread-safety: a single Agent instance is NOT safe for concurrent run_turn()
// calls — mTranscript and mSessionId are unprotected. Concurrency is achieved
// by one Agent per subagent thread; all agents share the OllamaClient singleton
// whose work queue serialises Ollama calls.
struct Agent {
  // `policy` is borrowed and must outlive the agent. `id` is the agent's node
  // id in AgentPool; the root passes its register_root() id and an empty
  // parent, subagents are constructed by AgentPool::spawn().
  Agent(AgentOptions options, const PolicyInterface& policy, std::string id,
        std::string parent_id, int depth);

  // Runs one turn. `objective` is the task; the return's `conclusion` is the
  // model's final answer. The scalar result (no children) is also pushed into
  // this agent's AgentPool node.
  AgentResult run_turn(const std::string& objective);

  // Loads a stored session for display only. It cannot restore the transcript
  // (sessions hold just the result tree), so mTranscript and mSessionId are
  // left untouched and a continued conversation writes a fresh session file.
  SessionResult resume(const std::string& session_id = "");

  // Writes the result tree out. A no-op for subagents (mDepth != 0); the root
  // calls this after every turn, so a crash costs at most the turn in flight.
  SessionStoreResult save() const;

  const std::string& session_id() const { return mSessionId; }
  const std::string& id() const { return mId; }
  int depth() const { return mDepth; }
  const std::vector<ChatMessage>& transcript() const { return mTranscript; }

  // Current contents of kMemoryPath; empty when the model hasn't written any.
  std::string memory() const;

  // Drops the transcript and starts a new session id. Memory survives, since
  // it is the part meant to outlive a conversation.
  void reset();

  // The tool schemas handed to the model, in advertised order.
  static std::vector<std::string> tool_schemas();
  // Just the names, for the system prompt.
  static std::vector<std::string> tool_names();

 protected:
  // Runs one tool call, policy first. Never throws and never reports failure as
  // anything but a ToolResult: a tool that fails is information the model
  // needs, not a reason to abandon the turn.
  ToolResult dispatch(const std::string& tool_name, const ToolArgs& args);

  // The system message, rebuilt for every model call so a memory update takes
  // effect on the very next step. Deliberately not stored in the transcript.
  std::string system_prompt() const;

 private:
  // Stamps the event with this agent's id/parent/depth/label and forwards it
  // to the process-wide AgentPool observer.
  void emit(AgentEvent event) const;

  AgentOptions mOptions;
  const PolicyInterface& mPolicy;
  SessionStore mStore;
  std::vector<ChatMessage> mTranscript;
  std::string mSessionId;
  std::string mId;
  std::string mParentId;
  int mDepth = 0;
  std::string mLabel;
};

}  // namespace agent

#endif  // AGENT_H
