#ifndef M8_UTIL_PROCESS_H
#define M8_UTIL_PROCESS_H

#include <signal.h>
#include <sys/types.h>

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

}  // namespace util

#endif  // M8_UTIL_PROCESS_H
