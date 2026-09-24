#include <cctype>
#include <cstdlib>

#include <sstream>
#include <string>
#include <vector>

#include <sp_memory.h>

namespace sp {

namespace {

std::string trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end and
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin and
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

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



Command parse_command(const std::string& input) {
  Command command;
  const std::string text = trim(input);
  if (text.empty() or text[0] != '/') return command;

  const size_t space = text.find_first_of(" \t");
  const std::string name = text.substr(0, space);
  const std::string args =
      space == std::string::npos ? std::string() : trim(text.substr(space + 1));

  if (name == "/quit") {
    command.kind = Command::Kind::Quit;
  } else if (name == "/help") {
    command.kind = Command::Kind::Help;
  } else if (name == "/reset") {
    command.kind = Command::Kind::Reset;
  } else if (name == "/remember") {
    command.kind = Command::Kind::Remember;
    command.args = args;
  } else if (name == "/memories") {
    command.kind = Command::Kind::Memories;
    command.args = args;
  } else if (name == "/forget") {
    // strtoull would accept "12abc" and a negative id wraps around, so the
    // digits are checked before the conversion rather than after it.
    const bool digits =
        not args.empty() and
        args.find_first_not_of("0123456789") == std::string::npos;
    if (digits) {
      command.kind = Command::Kind::Forget;
      command.id = std::strtoull(args.c_str(), nullptr, 10);
      if (command.id == 0) command.kind = Command::Kind::BadForget;
    } else {
      command.kind = Command::Kind::BadForget;
    }
  }
  // An unrecognised /word stays Kind::None and goes to the agent as a task,
  // which is what sp did before any of these commands existed.
  return command;
}

std::string do_remember(agent::MemoryStore& store, const std::string& text) {
  if (text.empty()) {
    return "usage: /remember <something about you or how you like things "
           "done>";
  }
  agent::Memory memory;
  memory.content = text;
  // Semantic and tagged as a preference: what a person types by hand is a
  // standing fact about them, not something that happened. The context_id is
  // left at the tool's own default so the agent's recalls see these too.
  memory.memory_type = agent::kMemorySemantic;
  memory.importance = 0.8;
  memory.tags = {"preference"};

  const agent::RememberResult stored = store.remember(memory);
  if (not stored.ok) return stored.error;
  return "remembered as memory " + std::to_string(stored.id);
}

std::string do_search(agent::MemoryStore& store, const std::string& query) {
  if (query.empty()) {
    const agent::MemoryStats stats = store.stats();
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

  agent::RecallQuery recall;
  recall.query = query;
  recall.k = 10;

  const agent::RecallResult recalled = store.recall(recall);
  if (not recalled.ok) return recalled.error;
  if (recalled.memories.empty()) return "no memories matched '" + query + "'";

  std::ostringstream out;
  for (const agent::ScoredMemory& scored : recalled.memories) {
    out << "[" << scored.memory.id << "] " << scored.memory.memory_type << " ("
        << two_places(scored.score) << ") "
        << first_line(scored.memory.content, 100) << "\n";
  }
  out << "(/forget <id> to delete one)";
  return out.str();
}

std::string do_forget(agent::MemoryStore& store, uint64_t id) {
  const agent::StoreResult forgotten = store.forget({id});
  if (not forgotten.ok) return forgotten.error;
  return "forgot memory " + std::to_string(id);
}

}  // namespace sp
