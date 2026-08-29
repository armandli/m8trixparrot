#include <core/tools.h>

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

// True when `dir` carries a marker that makes it a workspace root.
bool has_project_marker(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  return fs::exists(dir / ".git", ec) or fs::exists(dir / ".m8trix", ec) or
         fs::exists(dir / "pyproject.toml", ec) or
         fs::exists(dir / "requirements.txt", ec);
}

// Prepends the venv's site-packages to the running interpreter's sys.path and
// exports VIRTUAL_ENV / PATH — the parts of "activating a venv" that reach
// in-process imports and any console script a python script shells out to.
// Idempotent: a repeat call for the same venv doesn't stack duplicates. The
// caller must hold the GIL.
void activate_venv_locked(const std::filesystem::path& venv_path) {
  const std::string venv_python = (venv_path / "bin" / "python3").string();
  const std::string site_packages = rtrim_newlines(run_shell_capture(
      shell_quote(venv_python) +
      " -c \"import sysconfig; print(sysconfig.get_path('purelib'))\" "
      "2>/dev/null"));
  if (site_packages.empty()) return;

  const py::str entry(site_packages);
  py::object sys_path = py::module_::import("sys").attr("path");
  if (sys_path.contains(entry)) sys_path.attr("remove")(entry);
  sys_path.attr("insert")(0, entry);

  ::setenv("VIRTUAL_ENV", venv_path.string().c_str(), 1);
  const std::string bin = (venv_path / "bin").string();
  const char* path = std::getenv("PATH");
  const std::string current = path != nullptr ? path : "";
  if (current != bin and current.rfind(bin + ":", 0) != 0) {
    ::setenv("PATH", (bin + ":" + current).c_str(), 1);
  }
}

// The interpreter the linked libpython was built from. NOT sys.executable — for
// an embedded interpreter that is the host binary (see test/integration/main.cpp),
// which cannot create a venv. Prefers the ABI-matching prefix python from
// sysconfig, falls back to python3 / python on PATH, and accepts a candidate
// only if it runs and reports this interpreter's (major, minor). Empty when
// nothing suitable turns up. The caller must hold the GIL.
std::string resolve_base_python_locked() {
  const py::object version_info =
      py::module_::import("sys").attr("version_info");
  const std::string want =
      std::to_string(version_info.attr("major").cast<int>()) + "." +
      std::to_string(version_info.attr("minor").cast<int>());

  std::vector<std::string> candidates;

  const py::module_ sysconfig = py::module_::import("sysconfig");
  const py::object bindir = sysconfig.attr("get_config_var")("BINDIR");
  const py::object short_version =
      sysconfig.attr("get_config_var")("py_version_short");
  if (not bindir.is_none() and not short_version.is_none()) {
    candidates.push_back((std::filesystem::path(bindir.cast<std::string>()) /
                          ("python" + short_version.cast<std::string>()))
                             .string());
  }

  const py::module_ shutil = py::module_::import("shutil");
  for (const char* name : {"python3", "python"}) {
    const py::object found = shutil.attr("which")(name);
    if (not found.is_none()) candidates.push_back(found.cast<std::string>());
  }

  for (const std::string& candidate : candidates) {
    std::error_code ec;
    if (not std::filesystem::is_regular_file(candidate, ec)) continue;
    const std::string got = rtrim_newlines(run_shell_capture(
        shell_quote(candidate) +
        " -c \"import sys; print('%d.%d' % sys.version_info[:2])\" 2>/dev/null"));
    if (got == want) return candidate;
  }
  return std::string();
}

// Initializes the embedded Python interpreter, defines the _m8_run helper, and
// activates this workspace's kVenvDir venv if it already exists — exactly once
// per process. After init the GIL is released so any agent thread can acquire
// it; it is retaken at process exit before finalization.
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

    // If this workspace already has a .m8trixenv, activate it now. Building it
    // when it is missing is create_workspace_venv()'s job (called from main()),
    // not the interpreter's — so a stray `python` tool call or a unit test
    // never writes a venv into whatever directory it happens to run in.
    struct Venv {
      Venv() {
        namespace fs = std::filesystem;
        const std::string root = find_workspace_root();
        if (root.empty()) return;
        const fs::path venv_path = fs::path(root) / kVenvDir;
        std::error_code ec;
        if (fs::exists(venv_path / "pyvenv.cfg", ec)) {
          activate_venv_locked(venv_path);
        }
      }
    } venv;

    py::gil_scoped_release release;
  } g;
}

}  // namespace

std::string find_workspace_root() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path dir = fs::current_path(ec);
  if (ec) return std::string();
  for (;;) {
    if (has_project_marker(dir)) return dir.string();
    const fs::path parent = dir.parent_path();
    if (parent.empty() or parent == dir) return std::string();
    dir = parent;
  }
}

void ensure_python_ready() { ensure_interpreter(); }

VenvBootstrap create_workspace_venv() {
  namespace fs = std::filesystem;
  ensure_interpreter();  // struct Venv already activates .m8trixenv if present

  VenvBootstrap result;
  const std::string root = find_workspace_root();
  if (root.empty()) {
    result.status = VenvBootstrap::Status::NotAProject;
    return result;
  }

  const fs::path venv_path = fs::path(root) / kVenvDir;
  result.venv_dir = venv_path.string();

  std::error_code ec;
  const bool already = fs::exists(venv_path / "pyvenv.cfg", ec);

  if (not already) {
    std::string base_python;
    {
      py::gil_scoped_acquire gil;
      base_python = resolve_base_python_locked();
    }
    if (base_python.empty()) {
      result.status = VenvBootstrap::Status::Failed;
      result.detail =
          "found no Python to build the venv with (checked sysconfig BINDIR "
          "and PATH for python3 / python)";
      return result;
    }

    const std::string output =
        run_shell_capture(shell_quote(base_python) + " -m venv " +
                          shell_quote(venv_path.string()) + " 2>&1");

    if (not fs::exists(venv_path / "pyvenv.cfg", ec)) {
      result.status = VenvBootstrap::Status::Failed;
      result.detail = rtrim_newlines(output);
      if (result.detail.empty()) {
        result.detail = "'" + base_python + " -m venv' created no pyvenv.cfg";
      }
      return result;
    }
  }

  {
    py::gil_scoped_acquire gil;
    activate_venv_locked(venv_path);
  }
  result.status = already ? VenvBootstrap::Status::AlreadyPresent
                          : VenvBootstrap::Status::Created;
  return result;
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
