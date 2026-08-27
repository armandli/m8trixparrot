#include <core/tools.h>

#include <optional>
#include <string>

#include <pybind11/embed.h>

#include <core/tools_util.h>

namespace py = pybind11;

namespace agent {

namespace {

// Initializes the embedded Python interpreter and defines the _m8_run helper
// exactly once per process. scoped_interpreter finalizes at program exit.
void ensure_interpreter() {
    static struct Guard {
        py::scoped_interpreter interp;
        Guard() {
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
    } g;
}

}  // namespace

std::string PythonTool::description() const {
    return R"json({"name":"python","description":"Execute a Python script in-process and return its captured stdout and stderr. Use for computation, data transformation, or anything better expressed in Python than bash.","parameters":{"type":"object","properties":{"script":{"type":"string","description":"Python script to execute"}},"required":["script"]}})json";
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
    try {
        output = py::globals()["_m8_run"](py::str(*script)).cast<std::string>();
    } catch (const py::error_already_set& e) {
        result.error = "python: interpreter error: " + std::string(e.what());
        return result;
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
