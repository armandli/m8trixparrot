#ifndef M8_UTIL_PROCESS_H
#define M8_UTIL_PROCESS_H

#include <signal.h>
#include <sys/types.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace util {

// ---------------------------------------------------------------------------
// Descriptor hygiene for processes that fork.
//
// m8 runs several kinds of children at once from different threads — the
// agent's bash_repl shells, popen()ed commands, MCP servers — and a pipe end
// that leaks into the wrong child is not harmless: an MCP server only sees EOF
// on its stdin once *every* copy of the write end is closed, so a bash that
// inherited one keeps the server alive until it is killed. Two defences: mark
// every descriptor we create close-on-exec, and have each child we fork close
// everything above stderr before it execs, which also covers descriptors other
// code created without the flag.
// ---------------------------------------------------------------------------

// Sets FD_CLOEXEC. False if fcntl failed.
bool set_cloexec(int fd);

// macOS: write() on this descriptor fails with EPIPE instead of raising SIGPIPE
// when the reader is gone (F_SETNOSIGPIPE). A no-op elsewhere; Linux callers
// block the signal around the write instead (ScopedSigpipeBlock).
void set_nosigpipe(int fd);

// One past the highest descriptor a forked child should bother closing.
// Computed in the parent, because sysconf/getrlimit are not async-signal-safe.
int fd_close_limit();

// Closes every descriptor >= `low` (up to `limit`). Async-signal-safe, for use
// in a child between fork() and exec().
void close_fds_from(int low, int limit);

// Blocks SIGPIPE on the calling thread for the object's lifetime, and on Linux
// swallows one that was raised meanwhile, so a write to a dead pipe returns
// EPIPE instead of killing the process. Ignoring SIGPIPE process-wide is NOT an
// option: an ignored signal stays ignored across exec, and every shell m8
// starts would then see `yes | head` fail with "Broken pipe" errors.
struct ScopedSigpipeBlock {
  ScopedSigpipeBlock();
  ~ScopedSigpipeBlock();
  ScopedSigpipeBlock(const ScopedSigpipeBlock&) = delete;
  ScopedSigpipeBlock& operator=(const ScopedSigpipeBlock&) = delete;

private:
  sigset_t mOld;
  bool mWasBlocked = false;
};

// For threads that do nothing but I/O on pipes (MCP transports): block SIGPIPE
// for the rest of the thread's life.
void block_sigpipe_on_this_thread();

// ---------------------------------------------------------------------------
// Spawning a child with three pipes (MCP stdio servers).
// ---------------------------------------------------------------------------

using EnvList = std::vector<std::pair<std::string, std::string>>;

// The environment a spawned server starts from. Not the whole of ours: every
// variable m8 was started with (API keys, cloud credentials) would otherwise
// go to every third-party server. The allowlist is what the official MCP SDKs
// pass — enough to find programs and behave like a normal process — and
// `overrides` (the server's configured env) is applied on top. With
// `inherit_all`, everything is passed instead.
EnvList child_environment(bool inherit_all, const EnvList& overrides);

// The value of `name` in `env`, or empty.
std::string env_lookup(const EnvList& env, std::string_view name);

// `command` resolved to an executable path: as is when it contains a '/'
// (relative to `cwd` when relative), otherwise searched on `path_env` — the
// CHILD's PATH, which is why posix_spawnp (which searches ours) is not used.
// Empty when nothing executable is found.
std::string find_executable(std::string_view command, std::string_view path_env,
                            std::string_view cwd = {});

struct SpawnOptions {
  std::string command;            // resolved with find_executable()
  std::vector<std::string> args;  // argv[1..]
  EnvList env;                    // the child's whole environment
  std::string cwd;                // empty: inherit ours
};

struct SpawnedProcess {
  pid_t pid = -1;  // also its process group id
  int stdin_fd = -1;   // ours to write; non-blocking, close-on-exec
  int stdout_fd = -1;  // ours to read; non-blocking, close-on-exec
  int stderr_fd = -1;  // ours to read; non-blocking, close-on-exec
};

// posix_spawn in a new process group, with default signal dispositions and an
// empty signal mask (whatever m8's threads block must not leak into a
// server), and with no descriptor of ours open beyond its three pipes.
bool spawn_process(const SpawnOptions& options, SpawnedProcess& out,
                   std::string& error);

// Non-blocking reap. True when the child has exited (status in `status`).
bool reap(pid_t pid, int& status);

// "exited with status 2" / "killed by signal 9".
std::string describe_exit(int status);

}  // namespace util

#endif  // M8_UTIL_PROCESS_H
