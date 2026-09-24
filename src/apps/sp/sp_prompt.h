#ifndef SP_PROMPT_H
#define SP_PROMPT_H

#include <string>

#include <core/system_prompt.h>

#include <sp_paths.h>

namespace sp {

// sp's whole system prompt. sp is a shell assistant, not a coding agent, and
// this says so once rather than contradicting a core preamble that said
// otherwise.
//
// Deliberately absent, because none of it applies to sp: the coding-agent
// preamble, the "You are the root agent (depth 0 of max 1) / 1 of 1 agent
// slots are free" sentence (sp runs with subagents off), the "you have no
// subagent tools" rule, and the cwd/git-repo/branch/status workspace block
// (sp is run from wherever the user is standing, and can run `pwd` when a task
// actually needs to know).
//
// The memory section is omitted entirely when memory is off, so the model is
// never told to call a tool it was not given.
//
// `paths` is resolved once at startup (sp_paths.h) rather than worked out by
// the model: where a script goes, where sources go, where the trash is, and
// whether the bin directory is on the user's PATH are all facts about the
// machine, and the PATH answer decides what sp has to tell the user
// afterwards.
std::string make_system_prompt(const agent::PromptFacts& facts,
                               const std::string& username,
                               const std::string& home,
                               const SpPaths& paths);

}  // namespace sp

#endif  // SP_PROMPT_H
