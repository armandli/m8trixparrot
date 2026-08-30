#ifndef M8TRIXSH_SHELL_INTEGRATION_H
#define M8TRIXSH_SHELL_INTEGRATION_H

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace m8sh {

// True when `resolved_shell` (an absolute path or a bare name) is zsh. The
// prompt tag and the ai-mode Enter capture are implemented as a zsh rc snippet,
// so a non-zsh shell runs in the pane without them.
bool shell_is_zsh(std::string_view resolved_shell);

// The prompt m8trixsh installs on the shell it spawns. `format` is a template
// where `%tag` (the mode indicator) and `%git` (a branch segment) are
// m8trixsh's own, and everything else is ordinary zsh prompt syntax (`%~`,
// `%F{...}`, ...). Empty fields fall back to the built-in defaults below.
struct PromptConfig {
  std::string format;
  std::string shell_tag;
  std::string ai_tag;
  std::string ask_tag;
};

inline constexpr std::string_view kDefaultPromptFormat =
    "%tag %F{green}\xe2\x9e\x9c%f  %F{cyan}%~%f %F{yellow}%git%f ";
inline constexpr std::string_view kDefaultShellTag = "%F{blue}[shell]%f";
inline constexpr std::string_view kDefaultAiTag = "%F{magenta}[m8trx]%f";
inline constexpr std::string_view kDefaultAskTag = "%F{magenta}[m8trx?]%f";

// A throwaway ZDOTDIR that runs the user's real zsh config and then layers
// m8trixsh's prompt, git segment, mode tag, and Enter-capture on top. The
// directory is created on construction and removed on destruction.
struct ShellIntegration {
  // Builds the integration directory. `ok()` is false afterwards (with
  // `error()` set) if it could not be created; the caller should then run the
  // shell without it.
  explicit ShellIntegration(const PromptConfig& prompt);
  ~ShellIntegration();

  ShellIntegration(const ShellIntegration&) = delete;
  ShellIntegration& operator=(const ShellIntegration&) = delete;

  bool ok() const { return mOk; }
  const std::string& error() const { return mError; }

  // Environment for ShellSession::start: points zsh at the throwaway ZDOTDIR
  // and tells the snippet where to read the current mode.
  std::vector<std::pair<std::string, std::string>> env() const {
    return {{"ZDOTDIR", mDir}, {"__M8SH_MODE_FILE", mModeFile}};
  }

  // Rewrites the mode file the snippet reads. `mode` is "shell", "ai", or
  // "ai-ask". Pair this with writing redraw_sequence() to the pty so an idle
  // prompt repaints immediately.
  void set_mode(std::string_view mode) const;

  // The bytes to write to the pty to make zsh re-render the prompt after
  // set_mode() (the snippet binds a ZLE widget to them).
  static std::string_view redraw_sequence() { return "\x1b[218~"; }

 private:
  std::string mDir;
  std::string mModeFile;
  bool mOk = false;
  std::string mError;
};

}  // namespace m8sh

#endif  // M8TRIXSH_SHELL_INTEGRATION_H
