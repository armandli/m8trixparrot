#ifndef SHELL_SESSION_H
#define SHELL_SESSION_H

#include <sys/types.h>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace agent {

// The shell start() will exec: `shell_override` when non-empty, otherwise
// $SHELL, then the first of /bin/zsh, /bin/bash, /bin/sh that is executable.
std::string resolve_shell(const std::string& shell_override);

// A single interactive `$SHELL` running on its own pseudo-terminal, plus a
// background thread that drains the pty. It does no terminal emulation — bytes
// in, bytes out — so it stays free of libvterm and FTXUI and can be unit
// tested on its own. m8trixsh pairs it with a TerminalEmulator on the UI side.
//
// Thread model: start()/write_bytes()/resize()/cwd()/the destructor run on the
// owner's thread; on_bytes and on_exit fire on the internal reader thread and
// must be thread-safe. The child is reaped only by this object (waitpid on its
// own pid, no SIGCHLD handler), so it coexists with the agent's popen-based
// `bash` tool.
struct ShellSession {
  ShellSession();
  ~ShellSession();

  ShellSession(const ShellSession&) = delete;
  ShellSession& operator=(const ShellSession&) = delete;

  // Raw pty output, as it arrives. Feed it straight to a terminal emulator.
  std::function<void(std::string_view bytes)> on_bytes;
  // The shell exited; `status` is the raw waitpid status.
  std::function<void(int status)> on_exit;

  // Forks the shell on a pty sized cols x rows. `shell_override` wins when
  // non-empty; otherwise $SHELL, then /bin/zsh, /bin/bash, /bin/sh. `extra_env`
  // is `setenv`'d in the child right before exec (so it reaches the shell but
  // not this process or its other subprocesses). Returns false and fills
  // `error` (when non-null) if the fork or exec setup fails. Call once.
  bool start(int cols, int rows, const std::string& shell_override,
             std::string* error,
             const std::vector<std::pair<std::string, std::string>>& extra_env =
                 {});

  // Writes to the shell's stdin (the pty master). Handles partial writes and
  // EINTR; large inputs are chunked.
  void write_bytes(std::string_view bytes);

  // TIOCSWINSZ on the pty. Call whenever the display area changes.
  void resize(int cols, int rows);

  int master_fd() const { return mMaster; }
  pid_t pid() const { return mChild; }
  bool running() const { return mExitStatus.load() < 0 and mChild > 0; }

  // The shell process's current working directory, or "" if it can't be read
  // (before start(), after exit, or on a platform without the lookup).
  // proc_pidinfo on macOS, /proc/<pid>/cwd on Linux.
  std::string cwd() const;

protected:
  void reader_loop();
  void stop_child();

private:
  int mMaster = -1;
  pid_t mChild = -1;
  std::thread mReader;
  std::atomic<bool> mStop{false};
  std::atomic<int> mExitStatus{-1};
};

}  // namespace agent

#endif  // SHELL_SESSION_H
