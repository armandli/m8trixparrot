#ifndef M8_TOOLS_BASH_REPL_H
#define M8_TOOLS_BASH_REPL_H

#include <sys/types.h>

#include <cstdint>
#include <string>

namespace tools {

// One long-lived `bash` talking over pipes, so state survives between calls:
// variables, cwd, exported environment, shell functions, background jobs. That
// is the whole point — the `bash` tool starts a fresh `bash -c` every call, so
// `X=1` in one call is gone by the next, and an agent can never build anything
// up in the shell itself.
//
// Pipes rather than a pty (which is what ShellSession gives): a pty echoes the
// command back and interleaves prompts and escape sequences, and recovering
// clean output from that needs a terminal emulator. A pipe carries exactly
// what the command wrote, which is what a tool result has to be.
//
// The shell starts on first use and inherits this process's whole environment.
// It runs with --norc --noprofile so its behaviour does not vary with whatever
// the user happens to have in their rc files.
//
// Not thread-safe: one session belongs to one Agent, and an Agent runs its
// tools on a single thread.
struct BashReplSession {
  BashReplSession();
  ~BashReplSession();

  BashReplSession(const BashReplSession&) = delete;
  BashReplSession& operator=(const BashReplSession&) = delete;

  struct Outcome {
    std::string output;      // stdout and stderr, with the protocol stripped.
    int exit_code = 0;
    std::string cwd;         // Where the shell stands now.
    bool timed_out = false;
    bool restarted = false;  // The shell was replaced; session state was lost.
    std::string error;       // Set only when the command could not be run.
  };

  // Runs `command` in the persistent shell. `timeout_seconds <= 0` means no
  // limit. A command that fails, is killed, or times out is still a successful
  // run: the status is part of the Outcome, not an error.
  Outcome run(const std::string& command, int64_t timeout_seconds);

  // Kills the shell and forgets it. The next run() starts a clean one.
  void restart();

  bool running() const { return mChild > 0; }

private:
  // Starts the shell and drains its startup, leaving the stream at a known
  // point. Returns false and sets `error` if it could not be started.
  bool start(std::string& error);

  // SIGHUP, briefly wait, SIGKILL — then reap. Safe to call when not running.
  void stop();

  // Reads until the end marker or `deadline_ms` from now (negative = forever).
  // `found` says which happened; on a marker, it is removed from mPending and
  // its fields land in `exit_code` / `cwd`.
  bool read_until_marker(int64_t deadline_ms, int& exit_code, std::string& cwd,
                         bool& shell_died);

  pid_t mChild = -1;
  int mStdin = -1;   // Write end: the shell reads commands from this.
  int mStdout = -1;  // Read end: the shell's stdout and stderr, merged.

  std::string mToken;     // Per-session, so output cannot fake the marker.
  std::string mMarker;    // "\n__M8_END_<token>__"
  std::string mCommandPath;  // Temp file each command is written to.
  std::string mPending;   // Bytes read but not yet consumed by a run().
};

}  // namespace tools

#endif  // M8_TOOLS_BASH_REPL_H