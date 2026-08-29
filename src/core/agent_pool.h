#ifndef AGENT_POOL_H
#define AGENT_POOL_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <core/agent.h>
#include <core/agent_result.h>
#include <core/policy.h>

namespace agent {

struct Agent;  // Node holds a unique_ptr<Agent>; agent_pool.cpp completes it.

// Process-wide registry of every agent, root and subagent. A Meyer's singleton
// like OllamaClient: one instance, created on first use.
//
// It owns the recursion — spawn() enforces the depth and agent-count caps and
// starts each subagent on its own detached thread; assemble_tree() walks the
// registry to build the nested AgentResult the root persists. It also carries
// the one observer every agent emits through, so a UI sees the whole tree
// without threading a callback down each level.
struct AgentPool {
  static AgentPool& instance();

  // Set the caps. Call once, before any spawn().
  static void configure(int max_agents, int max_depth);

  // The observer every agent emits through. Set once, before any spawn(); the
  // callback runs on arbitrary agent threads and must be thread-safe.
  void set_observer(AgentObserver observer);
  void emit(const AgentEvent& event) const;

  // Register the root agent (depth 0, no owned Agent). Returns its node id,
  // which the caller passes to the Agent constructor.
  std::string register_root(std::string label = "root");

  // Spawn a subagent under `parent_id` on a detached thread running one turn
  // with `objective`. Returns its id, or an error when a cap is hit (the model
  // should then do the work itself). Never blocks.
  SpawnResult spawn(const std::string& parent_id, const std::string& objective,
                    const PolicyInterface& policy, AgentOptions options);

  // Record an agent's scalar result (no children) and wake anything waiting on
  // it. For subagents this also marks the node finished and frees a slot.
  void set_result(const std::string& id, AgentResult scalar);

  // Block until the agent finishes, then return its scalar result.
  std::optional<AgentResult> wait_for(const std::string& id);

  // The nested result for the subtree rooted at `id`: the node's own result
  // plus assemble_tree() of each child. Blocks on any descendant still running.
  AgentResult assemble_tree(const std::string& id);

  int live_count() const;
  int total_spawned() const;
  int max_depth() const { return mMaxDepth; }
  int max_agents() const { return mMaxAgents; }

  ~AgentPool();

private:
  AgentPool() = default;

  // One agent's slot in the registry. Never moved (holds a mutex/cv) and never
  // erased, so a raw Node* handed out by find() stays valid for the process.
  struct Node {
    std::string id;
    std::string parent_id;
    std::string objective;
    std::string label;
    int depth = 0;
    std::vector<std::string> child_ids;
    std::unique_ptr<Agent> agent;  // owned for subagents; null for the root
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<AgentResult> result;  // scalar (no children)
    bool finished = false;
    ~Node();  // defined in agent_pool.cpp, where Agent is complete
  };

  Node* find(const std::string& id) const;  // caller holds mMapMutex

  mutable std::mutex mMapMutex;
  std::unordered_map<std::string, std::unique_ptr<Node>> mNodes;

  mutable std::mutex mObserverMutex;
  AgentObserver mObserver;

  std::atomic<int> mLive{0};
  std::atomic<int> mTotal{0};
  int mMaxAgents = 16;
  int mMaxDepth = 3;
};

}  // namespace agent

#endif  // AGENT_POOL_H
