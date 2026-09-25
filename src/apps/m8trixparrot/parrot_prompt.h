#ifndef PARROT_PROMPT_H
#define PARROT_PROMPT_H

#include <string>

#include <core/agent/system_prompt.h>

namespace parrot {

// m8trixparrot's whole system prompt: the recursive coding agent, with python,
// subagents, skills and a git workspace. This is the text that used to live in
// Agent::system_prompt() — m8trixparrot is the application it was written for,
// so it keeps it, and the other applications no longer pay for it.
//
// Handles both the root and a subagent (facts.depth > 0), since subagents
// inherit the builder through AgentOptions.
std::string make_system_prompt(const agent::PromptFacts& facts);

}  // namespace parrot

#endif  // PARROT_PROMPT_H
