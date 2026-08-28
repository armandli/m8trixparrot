#include <core/agent.h>

#include <optional>
#include <string>

#include <core/agent_pool.h>
#include <core/json_util.h>
#include <core/tools_util.h>

namespace agent {

std::string SubagentCreateTool::description() {
  return R"json({"name":"subagent_create","description":"Spawn a subagent to work on an independent objective on its own thread. Returns immediately with an id; call subagent_wait with that id to collect its conclusion. Refused when the depth or agent-count limit is reached - in that case, do the work yourself.","parameters":{"type":"object","properties":{"objective":{"type":"string","description":"The self-contained objective for the subagent"}},"required":["objective"]}})json";
}

ToolResult SubagentCreateTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> objective = string_arg(args, "objective");
  if (not objective or objective->empty()) {
    result.error =
        "subagent_create: missing required string argument 'objective'";
    return result;
  }

  const SpawnResult spawned =
      AgentPool::instance().spawn(parent_id, *objective, policy, options);
  if (not spawned.ok) {
    // A cap was hit; the model reads the reason and does the work itself.
    result.error = spawned.error;
    return result;
  }

  JsonWriter writer;
  writer.begin_object()
      .field("id", spawned.id)
      .field("status", "running")
      .end_object();
  result.ok = true;
  result.output = writer.str();
  return result;
}

std::string SubagentWaitTool::description() {
  return R"json({"name":"subagent_wait","description":"Block until the subagent with the given id finishes, then return its result. The result contains ok, objective, conclusion, error, and steps fields.","parameters":{"type":"object","properties":{"id":{"type":"string","description":"Subagent id returned by subagent_create"}},"required":["id"]}})json";
}

ToolResult SubagentWaitTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> id = string_arg(args, "id");
  if (not id or id->empty()) {
    result.error = "subagent_wait: missing required string argument 'id'";
    return result;
  }

  const std::optional<AgentResult> finished =
      AgentPool::instance().wait_for(*id);
  if (not finished) {
    result.error = "subagent_wait: no subagent with id '" + *id + "' exists";
    return result;
  }

  JsonWriter writer;
  writer.begin_object()
      .field("ok", finished->ok)
      .field("objective", finished->objective)
      .field("conclusion", finished->conclusion)
      .field("error", finished->error)
      .field("steps", static_cast<int64_t>(finished->steps))
      .end_object();
  result.ok = true;
  result.output = writer.str();
  return result;
}

}  // namespace agent
