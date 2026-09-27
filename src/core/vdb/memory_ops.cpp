#include <cstdint>

#include <sstream>
#include <string>

#include <core/vdb/memory_ops.h>

namespace vdb {

namespace {

// A memory's content is several lines by design (see the LESSON template in
// the system prompt); a listing shows the first one so one memory is one row.
std::string first_line(const std::string& text, size_t max_chars) {
  std::string line = text.substr(0, text.find('\n'));
  if (line.size() > max_chars) line = line.substr(0, max_chars - 3) + "...";
  return line;
}

// Two decimals, without dragging <iomanip> in for one call.
std::string two_places(double value) {
  std::string text = std::to_string(value);
  const size_t dot = text.find('.');
  if (dot != std::string::npos and text.size() > dot + 3) {
    text = text.substr(0, dot + 3);
  }
  return text;
}

std::string human_bytes(uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
  return std::to_string(bytes / (1024 * 1024)) + " MB";
}

}  // namespace

std::string do_remember(MemoryStore& store, const std::string& text) {
  if (text.empty()) {
    return "usage: /remember <something about you or how you like things "
           "done>";
  }
  Memory memory;
  memory.content = text;
  // Semantic and tagged as a preference: what a person types by hand is a
  // standing fact about them, not something that happened. The context_id is
  // left at the tool's own default so the agent's recalls see these too.
  memory.memory_type = kMemorySemantic;
  memory.importance = 0.8;
  memory.tags = {"preference"};

  const RememberResult stored = store.remember(memory);
  if (not stored.ok) return stored.error;
  return "remembered as memory " + std::to_string(stored.id);
}

std::string do_search(MemoryStore& store, const std::string& query) {
  if (query.empty()) {
    const MemoryStats stats = store.stats();
    std::ostringstream out;
    out << stats.total << " memories";
    if (stats.total > 0) {
      out << " (" << stats.semantic << " preference/fact, " << stats.procedural
          << " lesson, " << stats.episodic << " episodic";
      if (stats.other > 0) out << ", " << stats.other << " other";
      out << ")";
    }
    if (stats.dim > 0) out << ", " << stats.dim << "-dim";
    if (stats.file_bytes > 0) out << ", " << human_bytes(stats.file_bytes);
    out << "\n" << stats.path;
    if (stats.total == 0) out << "\n(nothing remembered yet)";
    return out.str();
  }

  RecallQuery recall;
  recall.query = query;
  recall.k = 10;

  const RecallResult recalled = store.recall(recall);
  if (not recalled.ok) return recalled.error;
  if (recalled.memories.empty()) return "no memories matched '" + query + "'";

  std::ostringstream out;
  for (const ScoredMemory& scored : recalled.memories) {
    out << "[" << scored.memory.id << "] " << scored.memory.memory_type << " ("
        << two_places(scored.score) << ") "
        << first_line(scored.memory.content, 100) << "\n";
  }
  out << "(/forget <id> to delete one)";
  return out.str();
}

std::string do_forget(MemoryStore& store, uint64_t id) {
  const StoreResult forgotten = store.forget({id});
  if (not forgotten.ok) return forgotten.error;
  return "forgot memory " + std::to_string(id);
}

}  // namespace vdb
