#include <core/sane_policy.h>

#include <cctype>

#include <algorithm>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <core/tools_util.h>

namespace agent {

namespace {

// Names that acquire privilege. Matched on the basename, so /usr/bin/sudo is
// caught along with sudo.
bool is_privilege_command(const std::string& base) {
  return base == "su" or base == "sudo" or base == "doas" or base == "pkexec";
}

// Commands that run another command, so the thing that matters is further
// along the line rather than in the usual command position.
bool is_wrapper_command(const std::string& base) {
  return base == "env" or base == "xargs" or base == "command" or
         base == "nohup" or base == "timeout" or base == "setsid" or
         base == "stdbuf" or base == "sudoedit" or base == "time";
}

bool is_shell_command(const std::string& base) {
  return base == "sh" or base == "bash" or base == "zsh" or base == "dash" or
         base == "ksh";
}

// Commands whose job is to put bytes somewhere. Their destination arguments
// get the same containment check as write/edit.
bool is_write_command(const std::string& base) {
  return base == "tee" or base == "dd" or base == "cp" or base == "mv" or
         base == "install" or base == "truncate" or base == "ln" or
         base == "mkdir" or base == "touch" or base == "rm" or
         base == "rmdir" or base == "chmod" or base == "chown";
}

// Destination is the last non-flag argument (cp/mv/install/ln); everything
// else in the list takes a list of destinations.
bool takes_last_argument(const std::string& base) {
  return base == "cp" or base == "mv" or base == "install" or base == "ln";
}

// Writing to the process's own streams is routine and harmless, and these
// aren't under any write root, so they need an explicit pass.
bool is_pseudo_device(const std::string& path) {
  return path == "/dev/null" or path == "/dev/stdout" or
         path == "/dev/stderr" or path == "/dev/tty" or path == "/dev/zero" or
         path.rfind("/dev/fd/", 0) == 0;
}

struct Token {
  enum struct Kind { Word, Operator, Redirect };

  Kind kind = Kind::Word;
  std::string text;
  bool quoted = false;  // Word only: had quotes, so it may be a nested script.
};

// Splits a command line far enough to tell command position from arguments and
// to find redirection targets. Not a shell parser — it tracks quoting and
// escapes so `echo "a > b"` isn't mistaken for a redirect, and treats anything
// that starts a new command as an operator.
std::vector<Token> tokenize(const std::string& command) {
  std::vector<Token> tokens;

  size_t i = 0;
  while (i < command.size()) {
    const char c = command[i];

    if (c == ' ' or c == '\t' or c == '\r') {
      ++i;
      continue;
    }

    // Operators that hand command position to whatever follows.
    if (c == ';' or c == '\n' or c == '(' or c == ')' or c == '`') {
      tokens.push_back({Token::Kind::Operator, std::string(1, c), false});
      ++i;
      continue;
    }
    if (c == '|' or c == '&') {
      std::string op(1, c);
      if (i + 1 < command.size() and command[i + 1] == c) {
        op += c;
        ++i;
      } else if (c == '&' and i + 1 < command.size() and
                 command[i + 1] == '>') {
        // &> and &>> redirect both streams.
        ++i;
        std::string redirect = "&>";
        if (i + 1 < command.size() and command[i + 1] == '>') {
          redirect += '>';
          ++i;
        }
        tokens.push_back({Token::Kind::Redirect, redirect, false});
        ++i;
        continue;
      }
      tokens.push_back({Token::Kind::Operator, op, false});
      ++i;
      continue;
    }
    if (c == '$' and i + 1 < command.size() and command[i + 1] == '(') {
      tokens.push_back({Token::Kind::Operator, "$(", false});
      i += 2;
      continue;
    }
    if (c == '>') {
      std::string op = ">";
      ++i;
      if (i < command.size() and (command[i] == '>' or command[i] == '|')) {
        op += command[i];
        ++i;
      }
      tokens.push_back({Token::Kind::Redirect, op, false});
      continue;
    }
    if (c == '<') {
      // Input redirection reads; it never creates a file worth refusing.
      ++i;
      if (i < command.size() and command[i] == '<') ++i;
      continue;
    }
    // A leading file descriptor, as in 2> or 2>>.
    if (std::isdigit(static_cast<unsigned char>(c)) and i + 1 < command.size() and
        command[i + 1] == '>') {
      i += 2;
      std::string op = ">";
      if (i < command.size() and command[i] == '>') {
        op += '>';
        ++i;
      }
      tokens.push_back({Token::Kind::Redirect, op, false});
      continue;
    }

    // A word, with quotes and escapes resolved.
    std::string word;
    bool quoted = false;
    while (i < command.size()) {
      const char w = command[i];
      if (w == ' ' or w == '\t' or w == '\r' or w == '\n' or w == ';' or
          w == '|' or w == '&' or w == '(' or w == ')' or w == '`' or
          w == '>' or w == '<') {
        break;
      }
      if (w == '\\' and i + 1 < command.size()) {
        word += command[i + 1];
        i += 2;
        continue;
      }
      if (w == '\'') {
        quoted = true;
        ++i;
        while (i < command.size() and command[i] != '\'') word += command[i++];
        if (i < command.size()) ++i;
        continue;
      }
      if (w == '"') {
        quoted = true;
        ++i;
        while (i < command.size() and command[i] != '"') {
          if (command[i] == '\\' and i + 1 < command.size()) {
            word += command[i + 1];
            i += 2;
            continue;
          }
          word += command[i++];
        }
        if (i < command.size()) ++i;
        continue;
      }
      word += w;
      ++i;
    }
    tokens.push_back({Token::Kind::Word, word, quoted});
  }

  return tokens;
}

std::string basename_of(const std::string& text) {
  const size_t slash = text.find_last_of('/');
  return slash == std::string::npos ? text : text.substr(slash + 1);
}

bool is_flag(const std::string& text) {
  return text.size() > 1 and text[0] == '-';
}

// Arguments a wrapper command takes before the command it runs: flags,
// VAR=VALUE assignments, and durations like `5` or `5s`.
bool is_wrapper_noise(const std::string& text) {
  if (text.empty()) return true;
  if (is_flag(text)) return true;
  if (std::isdigit(static_cast<unsigned char>(text[0]))) return true;
  const size_t equals = text.find('=');
  const size_t slash = text.find('/');
  return equals != std::string::npos and
         (slash == std::string::npos or equals < slash);
}

}  // namespace

// ---------------------------------------------------------------------------

SanePolicy::SanePolicy()
    : SanePolicy(std::filesystem::current_path().string()) {}

SanePolicy::SanePolicy(const std::string& workspace_root) {
  std::error_code ec;
  std::filesystem::path root =
      std::filesystem::weakly_canonical(workspace_root, ec);
  if (ec or root.empty()) {
    root = std::filesystem::absolute(workspace_root, ec).lexically_normal();
  }
  mWriteRoots.push_back(root);

  std::filesystem::path temp =
      std::filesystem::weakly_canonical(kPublicTempDir, ec);
  if (ec or temp.empty()) temp = std::filesystem::path(kPublicTempDir);
  if (temp != root) mWriteRoots.push_back(temp);
}

std::string SanePolicy::name() const { return "sane"; }

bool SanePolicy::path_allowed(const std::string& path) const {
  if (path.empty()) return true;  // The tool's own error to report.
  if (is_pseudo_device(path)) return true;

  std::filesystem::path target(path);
  if (target.is_relative()) {
    // Against the workspace root rather than the live cwd, so the boundary
    // doesn't shift under the policy.
    target = mWriteRoots.front() / target;
  }

  std::error_code ec;
  // Resolves . and .. and follows symlinks on the part that exists, which is
  // what stops a link inside the workspace from pointing out of it.
  std::filesystem::path resolved = std::filesystem::weakly_canonical(target, ec);
  if (ec or resolved.empty()) resolved = target.lexically_normal();

  for (const std::filesystem::path& root : mWriteRoots) {
    // Component-wise, so /home/u/work does not admit /home/u/workspace.
    auto [root_it, target_it] = std::mismatch(root.begin(), root.end(),
                                               resolved.begin(), resolved.end());
    if (root_it == root.end()) return true;
  }
  return false;
}

std::string SanePolicy::inspect_command(const std::string& command) const {
  // Nested shells re-enter this, so a cap keeps a crafted command from
  // recursing without end.
  static thread_local int depth = 0;
  if (depth > 3) return std::string();
  ++depth;
  struct DepthGuard {
    ~DepthGuard() { --depth; }
  } guard;

  const std::vector<Token> tokens = tokenize(command);

  bool command_position = true;
  bool expect_wrapped_command = false;
  std::string current_command;      // Basename of the command being read.
  std::vector<std::string> arguments;

  // Destinations of the command that just ended, checked when it ends so the
  // whole argument list is known (cp needs its last argument).
  const auto flush_write_targets = [&]() -> std::string {
    if (current_command.empty() or not is_write_command(current_command)) {
      return std::string();
    }

    std::vector<std::string> destinations;
    if (current_command == "dd") {
      for (const std::string& argument : arguments) {
        if (argument.rfind("of=", 0) == 0) destinations.push_back(argument.substr(3));
      }
    } else if (takes_last_argument(current_command)) {
      for (auto it = arguments.rbegin(); it != arguments.rend(); ++it) {
        if (not is_flag(*it)) {
          destinations.push_back(*it);
          break;
        }
      }
    } else {
      for (const std::string& argument : arguments) {
        if (not is_flag(argument)) destinations.push_back(argument);
      }
    }

    for (const std::string& destination : destinations) {
      if (path_allowed(destination)) continue;
      return "`" + current_command + "` would write to " + destination +
             ", which is outside the workspace";
    }
    return std::string();
  };

  for (size_t i = 0; i < tokens.size(); ++i) {
    const Token& token = tokens[i];

    if (token.kind == Token::Kind::Operator) {
      if (const std::string error = flush_write_targets(); not error.empty()) {
        return error;
      }
      current_command.clear();
      arguments.clear();
      command_position = true;
      expect_wrapped_command = false;
      continue;
    }

    if (token.kind == Token::Kind::Redirect) {
      // The next word is where the output lands.
      if (i + 1 < tokens.size() and tokens[i + 1].kind == Token::Kind::Word) {
        const std::string& target = tokens[i + 1].text;
        ++i;
        // >&1 and >&2 name descriptors, not files.
        if (not target.empty() and target[0] != '&' and
            not path_allowed(target)) {
          return "redirecting output to " + target +
                 " would write outside the workspace";
        }
      }
      continue;
    }

    const std::string base = basename_of(token.text);

    if (command_position or expect_wrapped_command) {
      if (expect_wrapped_command and is_wrapper_noise(token.text)) {
        continue;  // Still looking for the command the wrapper runs.
      }

      if (is_privilege_command(base)) {
        return "`" + base +
               "` is not permitted: this agent does not escalate privileges";
      }

      if (is_wrapper_command(base)) {
        expect_wrapped_command = true;
        command_position = false;
        current_command = base;
        continue;
      }

      // `sh -c '...'` hides a whole command inside one argument.
      if (is_shell_command(base)) {
        for (size_t j = i + 1; j < tokens.size(); ++j) {
          if (tokens[j].kind != Token::Kind::Word) break;
          if (tokens[j].text == "-c" and j + 1 < tokens.size() and
              tokens[j + 1].kind == Token::Kind::Word) {
            const std::string nested = inspect_command(tokens[j + 1].text);
            if (not nested.empty()) return nested;
            break;
          }
        }
      }

      current_command = base;
      arguments.clear();
      command_position = false;
      expect_wrapped_command = false;
      continue;
    }

    arguments.push_back(token.text);
  }

  return flush_write_targets();
}

PolicyResult SanePolicy::verify(std::string_view tool_name,
                                      const ToolArgs& args) const {
  if (tool_name == "bash") {
    const std::optional<std::string> command = string_arg(args, "command");
    if (not command) return PolicyResult::allow();
    const std::string problem = inspect_command(*command);
    if (problem.empty()) return PolicyResult::allow();
    return PolicyResult::deny(
        problem +
        ". Files may only be written under the working directory or /tmp, and "
        "privilege escalation is refused outright. Use the `write` and `edit` "
        "tools for files inside the workspace.");
  }

  if (tool_name == "write" or tool_name == "edit") {
    const std::optional<std::string> path = string_arg(args, "path");
    // No path at all is the tool's own error to report, with a better message
    // than a policy could give.
    if (not path) return PolicyResult::allow();
    if (path_allowed(*path)) return PolicyResult::allow();

    std::string roots;
    for (const std::filesystem::path& root : mWriteRoots) {
      if (not roots.empty()) roots += " or ";
      roots += root.string();
    }
    return PolicyResult::deny("`" + std::string(tool_name) + "` to " + *path +
                              " is outside the writable area; only paths under " +
                              roots + " may be modified");
  }

  // Reading, searching and listing are unrestricted.
  return PolicyResult::allow();
}

}  // namespace agent
