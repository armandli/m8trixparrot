#ifndef TOOLS_H
#define TOOLS_H

#include <cstdint>
#include <functional>
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

// The dedicated Python virtual environment create_workspace_venv() builds at
// the workspace root (find_workspace_root()). python scripts and
// package_install both target this venv rather than whatever environment the
// m8trixparrot binary itself happens to be running under, so installs land in
// a sandbox scoped to this workspace instead of mutating a shared/dev venv.
inline constexpr const char* kVenvDir = ".m8trixenv";

// The nearest ancestor of the current working directory (inclusive) that
// carries a project marker — .git, .m8trix, pyproject.toml, or
// requirements.txt. Empty when the cwd sits under none of them (a bare scratch
// directory), which is the signal to skip the .m8trixenv bootstrap entirely.
std::string find_workspace_root();

// The outcome of create_workspace_venv(). `venv_dir` is the path that was or
// would be created (empty for NotAProject); `detail` explains a Failed result.
struct VenvBootstrap {
  enum class Status : int { Created, AlreadyPresent, NotAProject, Failed };
  Status status = Status::Failed;
  std::string venv_dir;
  std::string detail;
};

// Builds .m8trixenv at find_workspace_root() with a real ABI-compatible base
// Python (the embedded interpreter's own sys.executable is the host binary, so
// it can't be used), then activates it for this process — prepends its
// site-packages to sys.path and exports VIRTUAL_ENV / PATH. Call once from
// main() right after argv parsing, on the main thread; it brings the
// interpreter up as a side effect. A Failed result should stop startup with a
// message; a NotAProject result is normal (scratch dir) and means "carry on
// against the base interpreter".
VenvBootstrap create_workspace_venv();

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
// picked. Tools never parse JSON: use args_from_json() in tools_util.h to
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

// A stub: description() is real and the arguments are validated, but no search
// backend has been chosen yet, so execute() always reports that web search is
// unimplemented. Don't wire this into an agent loop expecting results.
struct WebSearchTool {
  std::string description() const;
  // query (string, required), limit (number, optional).
  ToolResult execute(const ToolArgs& args) const;
};

// Brings the embedded Python interpreter up (idempotent) and, when this
// workspace already has a .m8trixenv, activates it. Call once from the main
// thread at startup: an agent runs its tools on its own thread, and the
// interpreter must be initialised — and its GIL released — from the main thread
// before any of those threads touch Python. Also sets PIP_NO_INDEX /
// PIP_NO_INPUT process-wide so a script cannot install packages. Building the
// venv when it is missing is create_workspace_venv()'s job, not this one's.
void ensure_python_ready();

// Executes a Python script in-process via pybind11 embedding. stdout and
// stderr are captured and returned as the tool output; exceptions are caught
// inside Python and appended to the capture rather than propagated. Safe to
// call from any thread: the GIL is acquired and a mutex serialises the runs.
// The standard library and already-installed packages are importable;
// installing new packages is disabled (see ensure_python_ready).
struct PythonTool {
  std::string description() const;
  // script (string, required).
  ToolResult execute(const ToolArgs& args) const;
};

// Installs a single package by name, deduplicated through the process-wide
// PackageInstaller singleton (package_installer.h) so concurrent subagents
// asking for the same package produce one `pip install` rather than one each.
struct PackageInstallTool {
  std::string description() const;
  // package (string, required).
  ToolResult execute(const ToolArgs& args) const;
};

// Asks the operator a question and blocks until they answer. Stateful, like
// SkillTool: constructed at the dispatch site with a reference to the handler
// held in AgentOptions, which must outlive the call. `ask` runs on the agent's
// own thread, so a slow human only stalls that one turn.
struct AskUserTool {
  const std::function<std::string(const std::string&)>& ask;

  std::string description() const;
  // prompt (string, required).
  ToolResult execute(const ToolArgs& args) const;
};

}  // namespace agent

#endif  // TOOLS_H
