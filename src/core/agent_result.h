#ifndef AGENT_RESULT_H
#define AGENT_RESULT_H

#include <string>
#include <vector>

namespace agent {

// What an agent hands back when its turn is over: "task in, final message out."
// `objective` is the exact string the agent was asked to work on; `conclusion`
// is its final reply, the one message with no tool call that ended the turn.
// `children` carries the same record for every subagent spawned underneath,
// nested — so the root's result is the whole recursion as one tree.
//
// This lives in its own header, not agent.h or agent_pool.h, because
// session_store.h holds an AgentResult by value and agent.h includes
// session_store.h; anywhere else would be an include cycle.
struct AgentResult {
  bool ok = false;
  std::string objective;
  std::string conclusion;
  std::string error;
  int steps = 0;
  bool hit_step_limit = false;
  std::vector<AgentResult> children;
};

// The outcome of AgentPool::spawn(). On success `id` names the new subagent;
// on failure `error` is a message the model can act on (a depth or agent-count
// cap was hit, so it should do the work itself).
struct SpawnResult {
  bool ok = false;
  std::string id;
  std::string error;
};

}  // namespace agent

#endif  // AGENT_RESULT_H
