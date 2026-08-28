#include <core/agent_pool.h>

#include <system_error>
#include <thread>
#include <utility>

#include <core/agent.h>
#include <core/session_store.h>  // generate_uuid_v4

namespace agent {

namespace {

// One-line status for the SubagentDone event's summary.
std::string subagent_status(const AgentResult& result) {
  if (result.hit_step_limit) {
    return "step limit \xc2\xb7 " + std::to_string(result.steps) + " steps";
  }
  if (not result.ok) return "failed";
  return "ok \xc2\xb7 " + std::to_string(result.steps) +
         (result.steps == 1 ? " step" : " steps");
}

}  // namespace

// Defined here, where Agent is complete, so unique_ptr<Agent> in Node can be
// destroyed and the map of Nodes torn down.
AgentPool::Node::~Node() = default;
AgentPool::~AgentPool() = default;

AgentPool& AgentPool::instance() {
  static AgentPool pool;
  return pool;
}

void AgentPool::configure(int max_agents, int max_depth) {
  AgentPool& pool = instance();
  pool.mMaxAgents = max_agents;
  pool.mMaxDepth = max_depth;
}

void AgentPool::set_observer(AgentObserver observer) {
  std::lock_guard<std::mutex> lock(mObserverMutex);
  mObserver = std::move(observer);
}

void AgentPool::emit(const AgentEvent& event) const {
  std::lock_guard<std::mutex> lock(mObserverMutex);
  if (mObserver) mObserver(event);
}

AgentPool::Node* AgentPool::find(const std::string& id) const {
  const auto it = mNodes.find(id);
  return it == mNodes.end() ? nullptr : it->second.get();
}

std::string AgentPool::register_root(std::string label) {
  const std::string id = generate_uuid_v4();
  auto node = std::make_unique<Node>();
  node->id = id;
  node->depth = 0;
  node->label = std::move(label);

  std::lock_guard<std::mutex> lock(mMapMutex);
  mNodes.emplace(id, std::move(node));
  return id;
}

SpawnResult AgentPool::spawn(const std::string& parent_id,
                             const std::string& objective,
                             const PolicyInterface& policy,
                             AgentOptions options) {
  int parent_depth = 0;
  {
    std::lock_guard<std::mutex> lock(mMapMutex);
    if (Node* parent = find(parent_id)) parent_depth = parent->depth;
  }
  const int child_depth = parent_depth + 1;

  if (child_depth > mMaxDepth) {
    return {false, "",
            "subagent depth limit (" + std::to_string(mMaxDepth) +
                ") reached; complete this objective yourself"};
  }

  Node* np = nullptr;
  std::string id;
  {
    std::lock_guard<std::mutex> lock(mMapMutex);
    if (mLive.load() >= mMaxAgents) {
      return {false, "",
              "agent budget (" + std::to_string(mMaxAgents) +
                  ") exhausted; complete this objective yourself"};
    }

    id = generate_uuid_v4();
    auto node = std::make_unique<Node>();
    node->id = id;
    node->parent_id = parent_id;
    node->objective = objective;
    node->label = "subagent";
    node->depth = child_depth;
    node->agent =
        std::make_unique<Agent>(std::move(options), policy, id, parent_id,
                                child_depth);
    np = node.get();
    mNodes.emplace(id, std::move(node));
    if (Node* parent = find(parent_id)) parent->child_ids.push_back(id);
    mLive.fetch_add(1);
    mTotal.fetch_add(1);
  }

  {
    AgentEvent event;
    event.kind = AgentEvent::Kind::SubagentStart;
    event.summary = objective;
    event.agent_id = id;
    event.parent_id = parent_id;
    event.depth = child_depth;
    event.agent_label = "subagent";
    emit(event);
  }

  try {
    std::thread([this, np] {
      np->agent->run_turn(np->objective);  // calls set_result() before returning
      std::lock_guard<std::mutex> lock(mMapMutex);
      np->agent.reset();  // free the transcript; the Node and its result stay
    }).detach();
  } catch (const std::system_error& e) {
    AgentResult failed;
    failed.objective = objective;
    failed.error = std::string("could not start subagent thread: ") + e.what();
    set_result(id, std::move(failed));
    return {false, "", failed.error};
  }

  return {true, id, ""};
}

void AgentPool::set_result(const std::string& id, AgentResult scalar) {
  Node* node = nullptr;
  {
    std::lock_guard<std::mutex> lock(mMapMutex);
    node = find(id);
  }
  if (node == nullptr) return;

  AgentResult snapshot;
  int depth = 0;
  std::string node_id;
  std::string parent_id;
  std::string label;
  {
    std::lock_guard<std::mutex> lock(node->mutex);
    // The root keeps the objective from its first turn.
    if (node->result and not node->result->objective.empty()) {
      scalar.objective = node->result->objective;
    }
    // Free the slot before publishing the result, so a caller that wakes from
    // wait_for() sees an up-to-date live_count().
    if (node->depth > 0 and not node->finished) {
      node->finished = true;
      mLive.fetch_sub(1);
    }
    node->result = std::move(scalar);
    snapshot = *node->result;
    depth = node->depth;
    node_id = node->id;
    parent_id = node->parent_id;
    label = node->label;
  }
  node->cv.notify_all();

  if (depth > 0) {
    AgentEvent event;
    event.kind = AgentEvent::Kind::SubagentDone;
    event.text = snapshot.ok ? snapshot.conclusion : snapshot.error;
    event.summary = subagent_status(snapshot);
    event.agent_id = node_id;
    event.parent_id = parent_id;
    event.depth = depth;
    event.agent_label = label;
    event.ok = snapshot.ok;
    emit(event);
  }
}

std::optional<AgentResult> AgentPool::wait_for(const std::string& id) {
  Node* node = nullptr;
  {
    std::lock_guard<std::mutex> lock(mMapMutex);
    node = find(id);
  }
  if (node == nullptr) return std::nullopt;

  std::unique_lock<std::mutex> lock(node->mutex);
  node->cv.wait(lock, [&] { return node->result.has_value(); });
  return *node->result;
}

AgentResult AgentPool::assemble_tree(const std::string& id) {
  Node* node = nullptr;
  std::vector<std::string> child_ids;
  {
    std::lock_guard<std::mutex> lock(mMapMutex);
    node = find(id);
    if (node != nullptr) child_ids = node->child_ids;
  }

  AgentResult out;
  if (node == nullptr) return out;

  {
    std::unique_lock<std::mutex> lock(node->mutex);
    if (node->depth != 0) {
      node->cv.wait(lock, [&] { return node->result.has_value(); });
    }
    if (node->result) out = *node->result;
  }

  out.children.clear();
  for (const std::string& child_id : child_ids) {
    out.children.push_back(assemble_tree(child_id));
  }
  return out;
}

int AgentPool::live_count() const { return mLive.load(); }
int AgentPool::total_spawned() const { return mTotal.load(); }

}  // namespace agent
