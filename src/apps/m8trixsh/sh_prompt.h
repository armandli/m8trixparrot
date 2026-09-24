#ifndef SH_PROMPT_H
#define SH_PROMPT_H

#include <string>

#include <core/system_prompt.h>

namespace sh {

// m8trixsh's whole system prompt: the AI half of an interactive shell the
// human is also typing into, with the research -> plan -> approve -> script ->
// approve -> run workflow that `ask_user` exists for.
//
// This used to be workflow_prompt(), an anonymous-namespace function in
// main.cpp appended after a coding-agent preamble that contradicted its first
// sentence. It is now the entire prompt, and lives in its own translation unit
// so a test can read it.
std::string make_system_prompt(const agent::PromptFacts& facts,
                               const std::string& home);

}  // namespace sh

#endif  // SH_PROMPT_H
