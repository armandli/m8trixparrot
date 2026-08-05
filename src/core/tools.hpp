#ifndef TOOLS_H
#define TOOLS_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace agent {

// One tool-call argument value. The alternatives cover every parameter type
// that appears in config/basic_tools.json: strings, numbers, booleans, and
// edit's array of {oldText, newText} objects.
//
// JSON numbers reach us as either int64_t or double depending on how the model
// spelled them, so both are alternatives and int_arg() accepts either.
using ToolArgValue =
    std::variant<bool, int64_t, double, std::string, std::vector<std::string>,
                 std::vector<std::pair<std::string, std::string>>>;

// Argument name -> value. An absent key means the caller omitted an optional
// argument and the tool applies the default named in its schema. `std::less<>`
// so a lookup by string_view doesn't allocate.
using ToolArgs = std::map<std::string, ToolArgValue, std::less<>>;

// Where MemoryTool keeps the agent's working notes, relative to the working
// directory. A constant rather than a member so the tool stays memberless like
// the rest; BasicAgent reads the same path to fold memory into its prompt.
inline constexpr const char* kMemoryPath = ".mini_agent/memory.md";

struct ToolResult {
  bool ok = false;
  std::string output;  // What gets fed back to the model.
  std::string error;
  bool truncated = false;
  std::string overflow_path;  // Temp file holding the full output, if truncated.
};

// ---------------------------------------------------------------------------
// The tools.
//
// Every tool is stateless — no members, nothing to construct — and exposes the
// same two methods, so a caller dispatches on the tool name from a model's
// tool_call and hands over the same ToolArgs map regardless of which tool it
// picked. Tools never parse JSON: use args_from_json() in tools_util.hpp to
// turn a tool_call's argument object into a ToolArgs first.
//
// description() returns the tool's schema as a JSON object, in the shape
// ollama's /api/chat "tools" array expects — the same text as the tool's entry
// in config/basic_tools.json.
//
// A missing required argument, or one holding an unexpected type, comes back
// as ok = false with a message naming the argument. Nothing throws.
// ---------------------------------------------------------------------------

struct BashTool {
  std::string description() const;
  // command (string, required), timeout (number, seconds, optional).
  ToolResult execute(const ToolArgs& args) const;
};

struct ReadTool {
  std::string description() const;
  // path (string, required), offset (number, 1-indexed), limit (number).
  ToolResult execute(const ToolArgs& args) const;
};

struct WriteTool {
  std::string description() const;
  // path (string, required), content (string, required).
  ToolResult execute(const ToolArgs& args) const;
};

struct EditTool {
  std::string description() const;
  // path (string, required), edits (array of oldText/newText pairs, required).
  ToolResult execute(const ToolArgs& args) const;
};

struct FindTool {
  std::string description() const;
  // pattern (string, required), path (string), limit (number).
  ToolResult execute(const ToolArgs& args) const;
};

struct GrepTool {
  std::string description() const;
  // pattern (string, required), path, glob, ignoreCase, literal, context, limit.
  ToolResult execute(const ToolArgs& args) const;
};

// Fetches over the network with no restriction on scheme or host — a URL from
// model output can name loopback or a metadata endpoint just as easily as a
// public site. That is a deliberate choice, not an oversight: any guard belongs
// in whatever drives the tool.
struct WebFetchTool {
  std::string description() const;
  // url (string, required).
  ToolResult execute(const ToolArgs& args) const;
};

// Rewrites the agent's working memory at kMemoryPath. Overwrite rather than
// append is deliberate: the current memory is already in front of the model
// each turn, so it rewrites the whole thing and the file can't grow without
// bound.
struct MemoryTool {
  std::string description() const;
  // content (string, required).
  ToolResult execute(const ToolArgs& args) const;
};

// A stub: description() is real and the arguments are validated, but no search
// backend has been chosen yet, so execute() always reports that web search is
// unimplemented. Don't wire this into an agent loop expecting results.
struct WebSearchTool {
  std::string description() const;
  // query (string, required), limit (number, optional).
  ToolResult execute(const ToolArgs& args) const;
};

}  // namespace agent

#endif  // TOOLS_H
