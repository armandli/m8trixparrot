#include <core/tools.h>

#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

#include <pybind11/embed.h>

#include <core/tools_util.h>

namespace py = pybind11;

namespace agent {

namespace {

// _m8_run swaps the process-global sys.stdout / sys.stderr, so two scripts must
// not run at once even though each agent runs its tools on its own thread.
std::mutex& python_mutex() {
  static std::mutex mutex;
  return mutex;
}

// Initializes the embedded Python interpreter and defines the _m8_run helper
// exactly once per process. After init the GIL is released so any agent thread
// can acquire it; it is retaken at process exit before finalization.
//
// Member order matters: lockdown runs first (plain setenv, before anything
// Python), then interp holds the GIL, init runs while it is held, release drops
// it. Destruction is the reverse — release re-acquires the GIL, then interp
// finalizes with it held.
void ensure_interpreter() {
  static struct Guard {
    // The interpreter is not sandboxed, so a script can still shell out to pip.
    // Deny it a package index and any interactive prompt, so a script's own
    // `pip install X` fails fast ("No matching distribution found") instead of
    // mutating the environment. The `package_install` tool (package_installer.h)
    // is the one sanctioned installer: it unsets PIP_NO_INDEX for just its own
    // child process rather than for the interpreter as a whole.
    struct Lockdown {
      Lockdown() {
        ::setenv("PIP_NO_INDEX", "1", 1);
        ::setenv("PIP_NO_INPUT", "1", 1);
      }
    } lockdown;
    py::scoped_interpreter interp;
    struct Init {
      Init() {
        py::exec(R"(
def _m8_run(script_text):
    import sys, io, traceback
    buf = io.StringIO()
    old_out, old_err = sys.stdout, sys.stderr
    sys.stdout = sys.stderr = buf
    try:
        exec(compile(script_text, '<script>', 'exec'), {'__builtins__': __builtins__})
    except BaseException:
        buf.write(traceback.format_exc())
    finally:
        sys.stdout, sys.stderr = old_out, old_err
    return buf.getvalue()
)");
      }
    } init;
    py::gil_scoped_release release;
  } g;
}

}  // namespace

void ensure_python_ready() { ensure_interpreter(); }

std::string python_executable() {
  ensure_interpreter();
  std::lock_guard<std::mutex> lock(python_mutex());
  py::gil_scoped_acquire gil;
  return py::module_::import("sys").attr("executable").cast<std::string>();
}

std::string PythonTool::description() const {
    return R"json({"name":"python","description":"Execute a Python script in-process and return its captured stdout and stderr. Use for all computation, file I/O, data transformation, and anything scriptable. The Python standard library and any already-installed packages are available; a script cannot pip install new ones itself — use the package_install tool for that, then this tool can import it.","parameters":{"type":"object","properties":{"script":{"type":"string","description":"Python script to execute"}},"required":["script"]}})json";
}

ToolResult PythonTool::execute(const ToolArgs& args) const {
    ToolResult result;

    const std::optional<std::string> script = string_arg(args, "script");
    if (not script or script->empty()) {
        result.error = "python: missing required string argument 'script'";
        return result;
    }

    ensure_interpreter();

    std::string output;
    {
        std::lock_guard<std::mutex> lock(python_mutex());
        py::gil_scoped_acquire gil;
        try {
            output = py::globals()["_m8_run"](py::str(*script)).cast<std::string>();
        } catch (const py::error_already_set& e) {
            result.error = "python: interpreter error: " + std::string(e.what());
            return result;
        }
    }

    TruncatedOutput truncated = truncate_output(std::move(output), "python");
    result.ok = true;
    result.output = std::move(truncated.text);
    result.output += truncation_note(truncated);
    result.truncated = truncated.truncated;
    result.overflow_path = std::move(truncated.overflow_path);
    return result;
}

}  // namespace agent
