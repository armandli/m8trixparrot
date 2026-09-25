#include <core/tools/tools.h>

#include <optional>
#include <string>

#include <core/tools/bash_repl.h>
#include <core/tools/tools_util.h>

namespace tools {

std::string BashReplTool::description() {
  return R"json({"name":"bash_repl","description":"Run a bash command in a persistent shell. Unlike a one-shot shell, state survives between calls: variables, the working directory, exported environment, and shell functions are all still there on your next call, so you can build work up step by step instead of repeating setup. Background jobs with & are supported, but redirect their output to a file (cmd > /tmp/log 2>&1 &) or it will appear in a later call's output. Because it is one shell, a bare `exit` ends the session rather than the command; use (exit N) in a subshell if you need a non-zero status. Set restart to true to throw the shell away and start clean. Output truncated to 5000 lines/100KB; full output saved to a temp file if truncated.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"Command to execute"},"timeout":{"type":"number","description":"Timeout in seconds"},"restart":{"type":"boolean","description":"Discard the shell and its state before running; with no command, just resets"}},"required":[]}})json";
}

ToolResult BashReplTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<bool> restart = bool_arg(args, "restart");
  const std::optional<std::string> command = string_arg(args, "command");

  if (restart and *restart) session.restart();

  // A bare reset is a complete request: the agent asked for a clean shell and
  // got one.
  if (not command or command->empty()) {
    if (restart and *restart) {
      result.ok = true;
      result.output = "[shell restarted; session state cleared]";
      return result;
    }
    result.error = "bash_repl: missing required string argument 'command'";
    return result;
  }

  const std::optional<int64_t> timeout = int_arg(args, "timeout");
  const int64_t timeout_seconds = timeout and *timeout > 0 ? *timeout : 0;

  const BashReplSession::Outcome outcome =
      session.run(*command, timeout_seconds);

  // Only failing to run the command at all is an error; everything the command
  // then did is reportable content.
  if (not outcome.error.empty()) {
    result.error = outcome.error;
    return result;
  }

  TruncatedOutput truncated = truncate_output(outcome.output, "bash_repl");

  result.ok = true;
  result.output = std::move(truncated.text);
  result.output += truncation_note(truncated);

  if (not outcome.cwd.empty()) result.output += "\n[cwd: " + outcome.cwd + "]";

  if (outcome.timed_out) {
    result.output += "\n[command timed out after " +
                     std::to_string(timeout_seconds) + "s";
    result.output += outcome.restarted
                         ? "; it could not be interrupted, so the shell was "
                           "restarted and all session state was lost]"
                         : "; interrupted, session state preserved]";
  } else if (outcome.restarted) {
    result.output +=
        "\n[the shell exited, so a new one was started; all session state was "
        "lost]";
  } else if (outcome.exit_code != 0) {
    result.output +=
        "\n[command exited with status " + std::to_string(outcome.exit_code) +
        "]";
  }

  result.truncated = truncated.truncated;
  result.overflow_path = std::move(truncated.overflow_path);
  return result;
}

}  // namespace tools
