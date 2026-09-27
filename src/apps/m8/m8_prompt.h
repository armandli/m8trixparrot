#ifndef M8_PROMPT_H
#define M8_PROMPT_H

#include <string>
#include <vector>

#include <core/agent/system_prompt.h>

#include <m8_paths.h>

namespace m8 {

// m8's whole system prompt: a recursive coding agent whose only execution
// substrate is a persistent shell.
//
// The shape follows sp's builder rather than the old parrot one: core hands over
// the facts and this decides what to say about them, including saying nothing.
// What m8 says that no other application does is the *installed tools* block —
// m8 has no `read`, `write`, `edit` or `websearch` tool, it runs the tool_*
// binaries in `bash_repl` instead, and a model that is not told they exist will
// reinvent them with `cat` and `sed` or give up and ask the user.
//
// `installed` is the subset of those binaries main() actually found on PATH, so
// the prompt never advertises a command that would fail with "not found". An
// empty vector is a legitimate state (nothing installed yet) and the block is
// omitted entirely rather than left as a promise the machine cannot keep.
//
// Handles both the root and a subagent (facts.depth > 0), since AgentOptions —
// and therefore this builder — is copied by value into every subagent.
std::string make_system_prompt(const agent::PromptFacts& facts,
                               const M8Paths& paths,
                               const std::vector<std::string>& installed);

// The tool_* binaries m8 expects on PATH, paired with the one-line purpose the
// prompt gives each. Exposed so main() can probe for them and so a test can
// assert the prompt covers every one it was handed.
struct InstalledTool {
  const char* name;
  const char* purpose;
};
const std::vector<InstalledTool>& known_installed_tools();

}  // namespace m8

#endif  // M8_PROMPT_H
