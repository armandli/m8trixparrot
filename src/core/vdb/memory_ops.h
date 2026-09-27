#ifndef M8_VDB_MEMORY_OPS_H
#define M8_VDB_MEMORY_OPS_H

#include <cstdint>
#include <string>

#include <core/vdb/memory_store.h>

namespace vdb {

// The three memory operations a human can drive by hand from a TUI, behind the
// same MemoryStore the agent's `memory` tool uses. Each returns the line to show
// the user, errors included, so a TUI does no formatting of its own — and so a
// test can assert on the text without a screen.
//
// Taking the store by reference rather than MemoryOptions is what lets a test
// pass one built on hash_embedder and stay off the network. Callers get their
// store from MemoryStoreRegistry with the options the `memory` tool was
// configured with, so what the user sees here is what the agent sees: one
// RAM-resident handle per file, not two that would each go stale on the other's
// write.

std::string do_remember(MemoryStore& store, const std::string& text);
// An empty query reports statistics instead of searching.
std::string do_search(MemoryStore& store, const std::string& query);
std::string do_forget(MemoryStore& store, uint64_t id);

}  // namespace vdb

#endif  // M8_VDB_MEMORY_OPS_H
