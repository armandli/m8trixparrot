#ifndef TOOLS_UTIL_H
#define TOOLS_UTIL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/tools.hpp>

// libgit2's own type, forward-declared so <git2.h> stays out of this header.
struct git_repository;

namespace agent {

// The output caps every tool's schema promises: 5000 lines / 100KB.
inline constexpr size_t kMaxOutputLines = 5000;
inline constexpr size_t kMaxOutputBytes = 100000;

// ---------------------------------------------------------------------------
// Argument accessors.
//
// Each returns nullopt when the argument is absent *or* holds a type the tool
// can't use, so a tool only has to distinguish "usable value" from "not one" —
// the variant handling lives here and nowhere else.
// ---------------------------------------------------------------------------

std::optional<std::string> string_arg(const ToolArgs& args,
                                      std::string_view name);

// Accepts either the int64_t or the double alternative: a model may spell a
// count as `100` or `100.0` and both mean the same thing.
std::optional<int64_t> int_arg(const ToolArgs& args, std::string_view name);

std::optional<bool> bool_arg(const ToolArgs& args, std::string_view name);

// Null when absent or not the pair-array alternative. The pointer borrows from
// `args` and stays valid as long as it does.
const std::vector<std::pair<std::string, std::string>>* pairs_arg(
    const ToolArgs& args, std::string_view name);

// Turns a model tool_call's argument object into a ToolArgs. This is the only
// place JSON becomes arguments — the tools themselves never see JSON. Sets
// `error` and returns an empty map when `json` isn't a JSON object; keys whose
// values have no matching ToolArgValue alternative (null, nested objects) are
// skipped rather than treated as an error.
ToolArgs args_from_json(std::string_view json, std::string& error);

// ---------------------------------------------------------------------------
// Output truncation.
// ---------------------------------------------------------------------------

struct TruncatedOutput {
  std::string text;
  bool truncated = false;
  std::string overflow_path;  // Where the full text went; empty if not truncated.
};

// Clips `text` to the first `max_lines` lines and `max_bytes` bytes, whichever
// comes first, cutting on a line boundary. When it clips, the full text is
// written to a temp file named after `label` and its path returned, so the
// caller can point the model at the rest. A failed temp-file write is not an
// error: the text is still clipped, just with an empty overflow_path.
TruncatedOutput truncate_output(std::string text, std::string_view label,
                                size_t max_lines = kMaxOutputLines,
                                size_t max_bytes = kMaxOutputBytes);

// The note appended to truncated output, telling the model what it is missing.
std::string truncation_note(const TruncatedOutput& output);

// ---------------------------------------------------------------------------
// Filesystem walking.
// ---------------------------------------------------------------------------

// Answers "would git ignore this path?" for the repository containing
// `start_path`. Outside a repository nothing is ignored — that is a normal
// case, not an error — but `.git` itself is always ignored either way.
//
// Holds an open repository handle, so it lives as a local inside a tool's
// execute(); the tool structs themselves stay memberless.
struct IgnoreFilter {
  explicit IgnoreFilter(const std::string& start_path);
  ~IgnoreFilter();

  IgnoreFilter(const IgnoreFilter&) = delete;
  IgnoreFilter& operator=(const IgnoreFilter&) = delete;

  bool in_repo() const { return mRepo != nullptr; }

  // `path` may be absolute or relative to the process cwd. `is_directory`
  // matters because gitignore rules can end in '/' and only match directories.
  bool ignored(const std::string& path, bool is_directory) const;

private:
  struct RepoDeleter {
    void operator()(git_repository* repo) const;
  };

  std::unique_ptr<git_repository, RepoDeleter> mRepo;
  std::string mWorkdir;  // Absolute, no trailing slash. Empty if not in a repo.
};

// True when `head` (the first few KB of a file) looks like binary rather than
// text — a NUL byte, which no valid UTF-8 text file contains.
bool is_binary(std::string_view head);

// Reads a whole file into a string. Returns nullopt if it can't be opened.
std::optional<std::string> read_file(const std::string& path);

}  // namespace agent

#endif  // TOOLS_UTIL_H
