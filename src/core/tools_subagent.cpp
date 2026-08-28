#include <core/basic_agent.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <core/json_util.h>
#include <core/session_store.h>
#include <core/tools_util.h>

namespace agent {

std::string SubagentCreateTool::description() {
  return R"json({"name":"subagent_create","description":"Spawn a new subagent on its own thread with the given initial task. Returns immediately with a subagent ID; call subagent_wait with that ID to block until it finishes.","parameters":{"type":"object","properties":{"task":{"type":"string","description":"Initial task or context for the subagent"}},"required":["task"]}})json";
}

ToolResult SubagentCreateTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> task = string_arg(args, "task");
  if (not task or task->empty()) {
    result.error = "subagent_create: missing required string argument 'task'";
    return result;
  }

  const std::string id = generate_uuid_v4();
  auto record = std::make_shared<SubagentRecord>();
  record->agent = std::make_unique<BasicAgent>(options, policy);

  std::thread([record, task = *task]() {
    AgentTurnResult turn = record->agent->run_turn(task, nullptr);
    {
      std::lock_guard<std::mutex> lk(record->mutex);
      record->result = std::move(turn);
    }
    record->cv.notify_all();
  }).detach();

  {
    std::lock_guard<std::mutex> lk(mutex);
    subagents.emplace(id, std::move(record));
  }

  JsonWriter w;
  w.begin_object().field("id", id).field("status", "running").end_object();
  result.ok = true;
  result.output = w.str();
  return result;
}

std::string SubagentWaitTool::description() {
  return R"json({"name":"subagent_wait","description":"Block until the subagent with the given ID finishes, then return its result. The result contains ok, reply, error, and steps fields.","parameters":{"type":"object","properties":{"id":{"type":"string","description":"Subagent ID returned by subagent_create"}},"required":["id"]}})json";
}

ToolResult SubagentWaitTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> id = string_arg(args, "id");
  if (not id or id->empty()) {
    result.error = "subagent_wait: missing required string argument 'id'";
    return result;
  }

  std::shared_ptr<SubagentRecord> record;
  {
    std::lock_guard<std::mutex> lk(mutex);
    const auto it = subagents.find(*id);
    if (it == subagents.end()) {
      result.error = "subagent_wait: no subagent with id '" + *id + "' exists";
      return result;
    }
    record = it->second;
  }

  {
    std::unique_lock<std::mutex> lk(record->mutex);
    record->cv.wait(lk, [&] { return record->result.has_value(); });
  }

  const AgentTurnResult& turn = *record->result;
  JsonWriter w;
  w.begin_object()
      .field("ok", turn.ok)
      .field("reply", turn.reply)
      .field("error", turn.error)
      .field("steps", static_cast<int64_t>(turn.steps))
      .end_object();
  result.ok = true;
  result.output = w.str();
  return result;
}

}  // namespace agent
