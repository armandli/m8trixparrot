#ifndef SP_MEMORY_H
#define SP_MEMORY_H

#include <cstdint>
#include <string>

#include <core/vdb/memory_store.h>

// The parts of sp that are worth testing on their own: the slash-command
// grammar its TUI accepts and the three memory operations the user can drive
// by hand. main.cpp keeps the CLI, the FTXUI wiring and the agent loop, none
// of which a unit test can reach; sp's system prompt lives in sp_prompt.h.
namespace sp {

// One line of TUI input, classified. Anything that is not a recognised command
// is Kind::None and goes to the agent as a task — including an unknown /word,
// which keeps the old behaviour of letting the model deal with it.
struct Command {
  enum struct Kind : int {
    None,
    Quit,
    Help,
    Reset,
    Remember,   // `args` is the text; empty means the user typed no text.
    Memories,   // `args` is the query; empty means "show statistics".
    Forget,     // `id` is the memory to delete.
    BadForget,  // /forget with a missing or non-numeric argument.
  };

  Kind kind = Kind::None;
  std::string args;
  uint64_t id = 0;
};

Command parse_command(const std::string& input);

// ---------------------------------------------------------------------------
// The hand-driven memory operations, behind the same MemoryStore the agent's
// `memory` tool uses. Each returns the line to show the user, errors included,
// so the TUI does no formatting of its own — and so a test can assert on the
// text without an FTXUI screen. Taking the store by reference (rather than
// MemoryOptions) is what lets a test pass one built on hash_embedder and stay
// off the network.
// ---------------------------------------------------------------------------

std::string do_remember(vdb::MemoryStore& store, const std::string& text);
// An empty query reports statistics instead of searching.
std::string do_search(vdb::MemoryStore& store, const std::string& query);
std::string do_forget(vdb::MemoryStore& store, uint64_t id);

}  // namespace sp

#endif  // SP_MEMORY_H
