#include <core/util/process.h>

#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <time.h>
#endif

#include <algorithm>

namespace util {

bool set_cloexec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

void set_nosigpipe(int fd) {
#if defined(F_SETNOSIGPIPE)
  ::fcntl(fd, F_SETNOSIGPIPE, 1);
#else
  (void)fd;
#endif
}

int fd_close_limit() {
  // The soft limit is what the process could actually have open. Capped,
  // because some systems report it as effectively unlimited and a child
  // looping a billion close() calls would take seconds to start.
  constexpr long kCap = 65536;
  long limit = ::sysconf(_SC_OPEN_MAX);
  rlimit rl{};
  if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 and rl.rlim_cur != RLIM_INFINITY) {
    limit = std::max<long>(limit, static_cast<long>(rl.rlim_cur));
  }
  if (limit <= 0 or limit > kCap) limit = kCap;
  return static_cast<int>(limit);
}

void close_fds_from(int low, int limit) {
#if defined(__linux__) && defined(SYS_close_range)
  // One syscall instead of tens of thousands, where the kernel has it (5.9+).
  if (::syscall(SYS_close_range, static_cast<unsigned>(low), ~0U, 0) == 0) {
    return;
  }
#endif
  for (int fd = low; fd < limit; ++fd) ::close(fd);
}

ScopedSigpipeBlock::ScopedSigpipeBlock() {
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  sigemptyset(&mOld);
  ::pthread_sigmask(SIG_BLOCK, &block, &mOld);
  mWasBlocked = sigismember(&mOld, SIGPIPE) == 1;
}

ScopedSigpipeBlock::~ScopedSigpipeBlock() {
  if (mWasBlocked) return;  // the caller had it blocked already; leave it be
#if defined(__linux__)
  // A SIGPIPE raised by our write is pending on this thread; consume it before
  // unblocking, or it is delivered the moment the mask is restored.
  sigset_t pipe_only;
  sigemptyset(&pipe_only);
  sigaddset(&pipe_only, SIGPIPE);
  sigset_t pending;
  sigemptyset(&pending);
  if (::sigpending(&pending) == 0 and sigismember(&pending, SIGPIPE) == 1) {
    const timespec zero{0, 0};
    ::sigtimedwait(&pipe_only, nullptr, &zero);
  }
#endif
  // On macOS the descriptors that matter carry F_SETNOSIGPIPE, so nothing can
  // be pending here.
  ::pthread_sigmask(SIG_SETMASK, &mOld, nullptr);
}

void block_sigpipe_on_this_thread() {
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  ::pthread_sigmask(SIG_BLOCK, &block, nullptr);
}

}  // namespace util
