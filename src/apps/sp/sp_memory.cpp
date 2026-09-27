#include <cctype>
#include <cstdlib>

#include <sstream>
#include <string>
#include <vector>

#include <core/vdb/memory_ops.h>

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

// The three hand-driven operations moved into core/vdb so m8's TUI could offer
// the same commands over the same store. sp keeps the names it always had.
std::string do_remember(vdb::MemoryStore& store, const std::string& text) {
  return vdb::do_remember(store, text);
}

std::string do_search(vdb::MemoryStore& store, const std::string& query) {
  return vdb::do_search(store, query);
}

std::string do_forget(vdb::MemoryStore& store, uint64_t id) {
  return vdb::do_forget(store, id);
}

}  // namespace sp
