#include <core/tools.hpp>

#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

#include <optional>
#include <string>
#include <string_view>

#include <core/tools_util.hpp>

namespace agent {

namespace {

// Single-quotes `text` for /bin/sh, closing and reopening the quote around any
// embedded single quote. Used to hand a command to `timeout ... bash -c`.
std::string shell_quote(std::string_view text) {
  std::string quoted = "'";
  for (const char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

// Describes how the command ended, in the terms `sh` uses. popen's status is a
// wait(2) status, and 124 is the exit code `timeout` uses when it kills.
std::string exit_note(int status, bool has_timeout, int64_t timeout_seconds) {
  if (status == -1) return "\n[failed to reap the command]";

  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (exit_code == 0) return std::string();

  if (has_timeout and exit_code == 124) {
    return "\n[command timed out after " + std::to_string(timeout_seconds) +
           "s]";
  }
  if (WIFSIGNALED(status)) {
    return "\n[command killed by signal " + std::to_string(WTERMSIG(status)) +
           "]";
  }
  return "\n[command exited with status " + std::to_string(exit_code) + "]";
}

}  // namespace

std::string BashTool::description() const {
  return R"json({"name":"bash","description":"Execute a bash command in the working directory. Output truncated to 5000 lines/100KB; full output saved to a temp file if truncated.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"Command to execute"},"timeout":{"type":"number","description":"Timeout in seconds"}},"required":["command"]}})json";
}

ToolResult BashTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> command = string_arg(args, "command");
  if (not command or command->empty()) {
    result.error = "bash: missing required string argument 'command'";
    return result;
  }

  const std::optional<int64_t> timeout = int_arg(args, "timeout");
  const bool has_timeout = timeout and *timeout > 0;

  // stderr is merged into stdout: the model needs to see failures, and a
  // separate stream would only be reassembled out of order anyway.
  std::string invocation;
  if (has_timeout) {
    invocation = "timeout " + std::to_string(*timeout) + "s bash -c " +
                 shell_quote(*command) + " 2>&1";
  } else {
    invocation = "bash -c " + shell_quote(*command) + " 2>&1";
  }

  FILE* pipe = popen(invocation.c_str(), "r");
  if (pipe == nullptr) {
    result.error = "bash: failed to start the command";
    return result;
  }

  std::string output;
  char buffer[4096];
  size_t n = 0;
  while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    output.append(buffer, n);
  }
  const int status = pclose(pipe);

  TruncatedOutput truncated = truncate_output(std::move(output), "bash");

  // A non-zero exit isn't a tool failure — the command ran, and its status is
  // part of what the model asked for. Only failing to run it is an error.
  result.ok = true;
  result.output = std::move(truncated.text);
  result.output += truncation_note(truncated);
  result.output += exit_note(status, has_timeout, has_timeout ? *timeout : 0);
  result.truncated = truncated.truncated;
  result.overflow_path = std::move(truncated.overflow_path);
  return result;
}

}  // namespace agent
