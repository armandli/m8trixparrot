#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <core/memory_store.h>
#include <core/oc/ollama_client.h>
#include <core/tools/tools_util.h>

namespace agent {

namespace {

// Relative paths are resolved against the working directory, and an agent can
// change that; keying the registry on the canonical path is what keeps two
// spellings of one file from opening it twice.
std::string registry_key(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  return ec ? path : absolute.lexically_normal().string();
}

std::string format_memory(const ScoredMemory& scored) {
  const Memory& memory = scored.memory;
  std::string out = "[" + std::to_string(memory.id) + "] " +
                    memory.memory_type + " (score " +
                    std::to_string(scored.score).substr(0, 5) + ", importance " +
                    std::to_string(memory.importance).substr(0, 4);
  if (memory.context_id != "default") out += ", context " + memory.context_id;
  if (not memory.tags.empty()) {
    out += ", tags ";
    for (size_t i = 0; i < memory.tags.size(); ++i) {
      if (i > 0) out += "/";
      out += memory.tags[i];
    }
  }
  out += ")\n" + memory.content;
  return out;
}

}  // namespace

MemoryStoreRegistry& MemoryStoreRegistry::instance() {
  static MemoryStoreRegistry registry;
  return registry;
}

MemoryStore* MemoryStoreRegistry::get(const MemoryOptions& options,
                                      std::string& error) {
  const std::string key = registry_key(options.path);
  std::lock_guard<std::mutex> lock(mMutex);
  const auto found = mStores.find(key);
  if (found != mStores.end()) return found->second.get();

  MemoryOptions resolved = options;
  resolved.path = key;
  MemoryOpenResult opened = MemoryStore::open(resolved);
  if (not opened.ok) {
    error = opened.error;
    return nullptr;
  }
  MemoryStore* store = opened.store.get();
  mStores.emplace(key, std::move(opened.store));
  return store;
}

void MemoryStoreRegistry::reset() {
  std::lock_guard<std::mutex> lock(mMutex);
  mStores.clear();
}

bool memory_available(const std::string& model) {
  const oc::ShowResult shown = oc::OllamaClient::instance().show(model);
  if (not shown.ok) return false;
  return std::find(shown.capabilities.begin(), shown.capabilities.end(),
                   "embedding") != shown.capabilities.end();
}

// ───────────────────────────────── MemoryTool ──────────────────────────────

std::string MemoryTool::description() {
  return R"json({"name":"memory","description":"Long-term memory that survives across turns and sessions. action='remember' stores something with its embedding and returns an id; action='recall' finds the most relevant stored memories for a query, ranked by semantic similarity blended with how recent they are; action='forget' deletes one memory by id. Remember what the user tells you about themselves, decisions that were made, and conclusions you reached. Recall before answering anything that depends on earlier context.","parameters":{"type":"object","properties":{"action":{"type":"string","description":"remember: store a memory; recall: search memories; forget: delete a memory by id"},"content":{"type":"string","description":"For action='remember': the text to remember"},"type":{"type":"string","description":"For action='remember': episodic (something that happened), semantic (a fact), or procedural (how to do something). Default: episodic"},"context_id":{"type":"string","description":"For action='remember' or 'recall': the conversation or task these memories belong to"},"importance":{"type":"number","description":"For action='remember': 0.0 to 1.0, how much this matters. Default: 0.5"},"tags":{"type":"array","items":{"type":"string"},"description":"For action='remember': freeform labels; for action='recall': only memories carrying every one of these tags"},"query":{"type":"string","description":"For action='recall': what to search for"},"k":{"type":"number","description":"For action='recall': how many memories to return. Default: 5"},"min_importance":{"type":"number","description":"For action='recall': skip memories less important than this"},"filters":{"type":"string","description":"For action='recall': a JSON filter over the metadata, e.g. {\"memory_type\":{\"$eq\":\"semantic\"},\"importance\":{\"$gte\":0.7}}"},"recency_weight":{"type":"number","description":"For action='recall': 0.0 to 1.0, how much to favour recent memories over more similar ones. Default: 0.3"},"id":{"type":"number","description":"For action='forget': the memory id that remember returned"}},"required":["action"]}})json";
}

tools::ToolResult MemoryTool::execute(const tools::ToolArgs& args) const {
  tools::ToolResult result;

  const std::optional<std::string> action = tools::string_arg(args, "action");
  if (not action or action->empty()) {
    result.error = "memory: missing required string argument 'action'";
    return result;
  }

  std::string error;
  MemoryStore* store = MemoryStoreRegistry::instance().get(options, error);
  if (store == nullptr) {
    result.error = "memory: could not open the memory database: " + error;
    return result;
  }

  if (*action == "remember") {
    const std::optional<std::string> content = tools::string_arg(args, "content");
    if (not content or content->empty()) {
      result.error = "memory: action='remember' requires a 'content' argument";
      return result;
    }
    Memory memory;
    memory.content = *content;
    memory.memory_type = tools::string_arg(args, "type").value_or(kMemoryEpisodic);
    memory.context_id = tools::string_arg(args, "context_id").value_or("default");
    memory.importance =
        std::clamp(tools::double_arg(args, "importance").value_or(0.5), 0.0, 1.0);
    if (const std::vector<std::string>* tags = tools::strings_arg(args, "tags")) {
      memory.tags = *tags;
    }

    const RememberResult stored = store->remember(memory);
    if (not stored.ok) {
      result.error = stored.error;
      return result;
    }
    result.ok = true;
    result.output = "Remembered as memory " + std::to_string(stored.id) + ".";
    return result;
  }

  if (*action == "recall") {
    const std::optional<std::string> query = tools::string_arg(args, "query");
    if (not query or query->empty()) {
      result.error = "memory: action='recall' requires a 'query' argument";
      return result;
    }
    RecallQuery recall;
    recall.query = *query;
    const int64_t k = tools::int_arg(args, "k").value_or(5);
    recall.k = static_cast<size_t>(std::clamp<int64_t>(k, 1, 50));
    if (const std::optional<std::string> type = tools::string_arg(args, "type")) {
      recall.memory_type = *type;
    }
    if (const std::optional<std::string> context =
            tools::string_arg(args, "context_id")) {
      recall.context_id = *context;
    }
    if (const std::optional<double> floor =
            tools::double_arg(args, "min_importance")) {
      recall.min_importance = *floor;
    }
    if (const std::vector<std::string>* tags = tools::strings_arg(args, "tags")) {
      recall.tags = *tags;
    }
    recall.filters = tools::string_arg(args, "filters").value_or("");
    if (const std::optional<double> weight =
            tools::double_arg(args, "recency_weight")) {
      recall.recency_weight = std::clamp(*weight, 0.0, 1.0);
    }

    const RecallResult recalled = store->recall(recall);
    if (not recalled.ok) {
      result.error = recalled.error;
      return result;
    }
    if (recalled.memories.empty()) {
      result.ok = true;
      result.output = "No memories matched.";
      return result;
    }

    std::string out;
    for (const ScoredMemory& memory : recalled.memories) {
      out += format_memory(memory) + "\n\n";
    }
    tools::TruncatedOutput truncated = tools::truncate_output(std::move(out), "memory");
    result.ok = true;
    result.output = std::move(truncated.text);
    result.output += tools::truncation_note(truncated);
    result.truncated = truncated.truncated;
    result.overflow_path = std::move(truncated.overflow_path);
    return result;
  }

  if (*action == "forget") {
    const std::optional<int64_t> id = tools::int_arg(args, "id");
    if (not id or *id <= 0) {
      result.error = "memory: action='forget' requires an 'id' argument";
      return result;
    }
    const StoreResult forgotten =
        store->forget({static_cast<uint64_t>(*id)});
    if (not forgotten.ok) {
      result.error = "memory: " + forgotten.error;
      return result;
    }
    result.ok = true;
    result.output = "Forgot memory " + std::to_string(*id) + ".";
    return result;
  }

  result.error = "memory: unknown action '" + *action +
                 "'; expected remember, recall, or forget";
  return result;
}

}  // namespace agent
