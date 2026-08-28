#include <core/tools.h>

#include <cstdlib>
#include <filesystem>
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

std::string rtrim_newlines(std::string text) {
  while (not text.empty() and (text.back() == '\n' or text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

// Initializes the embedded Python interpreter, defines the _m8_run helper, and
// brings up its dedicated kVenvDir virtual environment — exactly once per
// process. After init the GIL is released so any agent thread can acquire it;
// it is retaken at process exit before finalization.
//
// Member order matters: lockdown runs first (plain setenv, before anything
// Python), then interp holds the GIL, init and venv run while it is held,
// release drops it. Destruction is the reverse — release re-acquires the GIL,
// then interp finalizes with it held.
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

    // Creates kVenvDir (relative to cwd) if it isn't already there, using the
    // embedded interpreter's own python so its compiled packages stay
    // ABI-compatible, then prepends its site-packages to sys.path so `python`
    // scripts can import whatever package_install puts there. Also exports
    // VIRTUAL_ENV and prepends the venv's bin/ to PATH — the rest of what
    // activating a venv normally does — so a script that shells out to a
    // venv-installed console script (anything but pip, which stays behind the
    // PIP_NO_INDEX lockdown) finds it. Failure at any step degrades silently
    // to running without a venv rather than aborting interpreter startup.
    struct Venv {
      Venv() {
        namespace fs = std::filesystem;

        const std::string interpreter_python =
            py::module_::import("sys").attr("executable").cast<std::string>();
        const fs::path venv_path = fs::absolute(kVenvDir);

        if (not fs::exists(venv_path / "pyvenv.cfg")) {
          run_shell_capture(shell_quote(interpreter_python) + " -m venv " +
                            shell_quote(venv_path.string()) + " 2>&1");
        }
        if (not fs::exists(venv_path / "pyvenv.cfg")) return;

        const std::string venv_python =
            (venv_path / "bin" / "python3").string();
        const std::string site_packages = rtrim_newlines(run_shell_capture(
            shell_quote(venv_python) +
            " -c \"import sysconfig; print(sysconfig.get_path('purelib'))\" "
            "2>/dev/null"));
        if (site_packages.empty()) return;

        py::module_::import("sys").attr("path").attr("insert")(0,
                                                                site_packages);
        ::setenv("VIRTUAL_ENV", venv_path.string().c_str(), 1);
        const char* path = std::getenv("PATH");
        ::setenv("PATH",
                ((venv_path / "bin").string() + ":" + (path ? path : ""))
                    .c_str(),
                1);
      }
    } venv;

    py::gil_scoped_release release;
  } g;
}

}  // namespace

void ensure_python_ready() { ensure_interpreter(); }

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
