#ifndef M8_TOOLS_PROTECTED_PATHS_H
#define M8_TOOLS_PROTECTED_PATHS_H

#include <filesystem>
#include <string>
#include <string_view>

namespace tools {

// Files the file tools refuse to touch, however the model spells the path.
//
// The list is hardcoded on purpose. A configurable one would have to live
// somewhere, and every candidate location is either inside the workspace the
// agent is editing or reachable from the shell it holds — at which point the
// list protects nothing. For the same reason there is no override: no flag, no
// environment variable, no config key. Anything an agent can set costs it
// exactly as little as it costs a human, so it would stop accidents and nothing
// else. When a protected file genuinely needs changing, the human changes it.
//
// TWO TIERS, because the danger is not the same in both directions:
//
//   Secret          reading it leaks a credential, so read, write and edit are
//                   all refused: ~/.ssh, ~/.aws, ~/.git-credentials, ~/.netrc.
//   ExecutionVector writing it gets code run later, but reading it is harmless
//                   and often useful, so only write and edit are refused:
//                   ~/.zshrc, .git/hooks, /etc, launchd and systemd units.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS NOT
//
// It is not a sandbox, and it is important not to mistake it for one.
//
// It is stronger than policy::SanePolicy in one specific way: it runs inside the
// tool, on the final resolved path, so it cannot be fooled by `eval`, base64, a
// variable, or a path assembled at runtime — whatever the caller did, the tool
// sees where the bytes are actually going.
//
// But it only guards the tools that call it. A shell can reach the same file
// with a redirect, sed -i, tee, cp, or an interpreter one-liner, and nothing
// here sees any of it. policy::SanePolicy consults this list too, which catches
// the straightforward shell forms under that policy — but m8trixsh and sp run
// YoloPolicy, where nothing is checked at all.
//
// Two limits are inherent to checking a path rather than a file descriptor: a
// hardlink to a protected file resolves to itself and is not detected, and there
// is a window between this check and the open() that follows it in which a
// symlink could be swapped. Both need shell access that could be used more
// directly anyway.
//
// So: this makes accidental destruction very unlikely and closes the route an
// agent reaches for first. A real boundary is the operating system's — a sandbox
// profile, a container, or a separate low-privilege user.
// ---------------------------------------------------------------------------

// What the caller is about to do, since the two tiers differ on reads.
enum struct PathAccess : int { Read, Write };

// Empty when the access is fine; otherwise the reason to refuse.
//
// The message is written for the model that is about to read it, and says three
// things deliberately: the resolved path (so a symlink or `..` shows what was
// actually hit), that there is no override, and what to do instead. Without the
// last part a model reads "permission problem" and reaches for `cat >` next,
// which is the outcome this exists to prevent.
//
// `tool_name` is the name the caller is known by — "write", "edit", "read".
std::string protected_path_reason(const std::string& path, PathAccess access,
                                  std::string_view tool_name);

// True when `path` is in the Secret tier. For callers that filter rather than
// refuse: `grep` and `find` walk whole directory trees, and skipping a file is
// the right answer there where failing the whole call is not.
bool is_protected_secret(const std::string& path);

// The resolution `protected_path_reason` performs before matching, exposed
// because it is the part with the interesting failure modes:
//
//   - a leading `~`, `~/` or `$HOME/` is expanded, which nothing else in this
//     repo does. bash expands an unquoted `~` before the tool ever sees it, but
//     a quoted one arrives literally, and a check that missed it would be
//     trivially bypassed.
//   - a relative path is resolved against the current directory.
//   - `weakly_canonical` normalizes `..` and follows symlinks on the part of the
//     path that exists. That is what catches a link pointing at a protected
//     file, and a link standing in for a protected parent directory.
//     `canonical` would be wrong: a write target usually does not exist yet.
//
// Never throws; falls back to a lexically normalized absolute path.
std::filesystem::path resolve_for_guard(const std::string& path);

// Writing to the process's own streams is routine and harmless, and none of
// these sit under any directory a guard would confine writes to, so they need an
// explicit pass. Shared with policy::SanePolicy, which had its own copy.
bool is_pseudo_device(const std::string& path);

// Just the tilde step of resolve_for_guard: a leading `~`, `~/` or `$HOME/`
// becomes $HOME, and anything else is returned unchanged. Split out because
// policy::SanePolicy needs this expansion but resolves what is left against the
// workspace root rather than the current directory, so it cannot use the whole
// of resolve_for_guard. With no usable $HOME the path is returned as it came.
std::string expand_home(const std::string& path);

}  // namespace tools

#endif  // M8_TOOLS_PROTECTED_PATHS_H
