#include <core/util/process.h>

#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <time.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>

extern char** environ;

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


namespace {

bool allowlisted(std::string_view name) {
  static constexpr const char* kExact[] = {
      "HOME",        "LOGNAME",     "PATH",       "SHELL",      "TERM",
      "USER",        "LANG",        "TMPDIR",     "TZ",         "http_proxy",
      "https_proxy", "no_proxy",    "HTTP_PROXY", "HTTPS_PROXY", "NO_PROXY",
      "ALL_PROXY",   "all_proxy",
  };
  for (const char* exact : kExact) {
    if (name == exact) return true;
  }
  // Locale and XDG directories: harmless, and plenty of tools misbehave
  // without them.
  return name.rfind("LC_", 0) == 0 or name.rfind("XDG_", 0) == 0;
}

bool is_executable_file(const std::string& path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) return false;
  if (not S_ISREG(info.st_mode)) return false;
  return ::access(path.c_str(), X_OK) == 0;
}

bool make_pipe(int fds[2]) {
#if defined(__linux__)
  return ::pipe2(fds, O_CLOEXEC) == 0;
#else
  // No pipe2 on macOS. The window between pipe() and the fcntl() is real, but
  // every child this process forks itself closes inherited descriptors, and
  // POSIX_SPAWN_CLOEXEC_DEFAULT covers the servers.
  if (::pipe(fds) != 0) return false;
  set_cloexec(fds[0]);
  set_cloexec(fds[1]);
  return true;
#endif
}

void set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void close_pair(int fds[2]) {
  for (int i = 0; i < 2; ++i) {
    if (fds[i] >= 0) ::close(fds[i]);
    fds[i] = -1;
  }
}

// `command`, a bare name, on the directories of `path_env`; the first match
// wins. An empty entry is the current directory (`cwd` when given). With
// `absolute_only`, that and every other relative entry are skipped instead.
std::string search_path(std::string_view command, std::string_view path_env,
                        std::string_view cwd, bool absolute_only) {
  size_t start = 0;
  while (start <= path_env.size()) {
    const size_t colon = path_env.find(':', start);
    std::string dir(path_env.substr(
        start, colon == std::string_view::npos ? std::string_view::npos
                                               : colon - start));
    const bool relative = dir.empty() or dir.front() != '/';
    if (not (absolute_only and relative)) {
      if (dir.empty()) dir = cwd.empty() ? "." : std::string(cwd);
      const std::string candidate = dir + "/" + std::string(command);
      if (is_executable_file(candidate)) return candidate;
    }
    if (colon == std::string_view::npos) break;
    start = colon + 1;
  }
  return std::string();
}

}  // namespace

EnvList child_environment(bool inherit_all, const EnvList& overrides) {
  EnvList env;
  for (char** entry = environ; entry != nullptr and *entry != nullptr; ++entry) {
    const std::string_view text(*entry);
    const size_t equals = text.find('=');
    if (equals == std::string_view::npos or equals == 0) continue;
    const std::string_view name = text.substr(0, equals);
    if (not inherit_all and not allowlisted(name)) continue;
    env.emplace_back(std::string(name), std::string(text.substr(equals + 1)));
  }
  for (const auto& [name, value] : overrides) {
    auto it = std::find_if(env.begin(), env.end(),
                           [&](const auto& pair) { return pair.first == name; });
    if (it != env.end()) {
      it->second = value;
    } else {
      env.emplace_back(name, value);
    }
  }
  return env;
}

std::string env_lookup(const EnvList& env, std::string_view name) {
  for (const auto& [key, value] : env) {
    if (key == name) return value;
  }
  return std::string();
}

std::string find_executable(std::string_view command, std::string_view path_env,
                            std::string_view cwd) {
  if (command.empty()) return std::string();
  if (command.find('/') != std::string_view::npos) {
    std::string path(command);
    if (path.front() != '/' and not cwd.empty()) {
      path = std::string(cwd) + "/" + path;
    }
    return is_executable_file(path) ? path : std::string();
  }
  return search_path(command, path_env, cwd, /*absolute_only=*/false);
}

std::string find_installed_executable(std::string_view command,
                                      std::string_view path_env) {
  if (command.empty() or command.find('/') != std::string_view::npos) {
    return std::string();
  }
  return search_path(command, path_env, {}, /*absolute_only=*/true);
}

bool spawn_process(const SpawnOptions& options, SpawnedProcess& out,
                   std::string& error) {
  // posix_spawn searches no PATH: a bare name would be a file of that name in
  // the current directory, usually the workspace. Callers resolve it first.
  if (options.command.find('/') == std::string::npos) {
    error = "could not start " + options.command + ": not resolved to a path";
    return false;
  }

  int in[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (not make_pipe(in) or not make_pipe(stdout_pipe) or
      not make_pipe(stderr_pipe)) {
    error = std::string("could not create pipes: ") + std::strerror(errno);
    close_pair(in);
    close_pair(stdout_pipe);
    close_pair(stderr_pipe);
    return false;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
  if (not options.cwd.empty()) {
#if defined(__APPLE__)
    if (__builtin_available(macOS 26.0, *)) {
      posix_spawn_file_actions_addchdir(&actions, options.cwd.c_str());
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());
#pragma clang diagnostic pop
    }
#else
    posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());
#endif
  }
#if defined(__linux__) && defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
  posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif

  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
                POSIX_SPAWN_SETSIGMASK;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
  // Only the three descriptors set up above survive into the child.
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&attributes, flags);
  posix_spawnattr_setpgroup(&attributes, 0);  // a group of its own
  sigset_t defaults;
  sigemptyset(&defaults);
  for (const int signal_number :
       {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD, SIGUSR1, SIGUSR2}) {
    sigaddset(&defaults, signal_number);
  }
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  sigset_t empty;
  sigemptyset(&empty);
  posix_spawnattr_setsigmask(&attributes, &empty);

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(options.command.c_str()));
  for (const std::string& arg : options.args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  std::vector<std::string> env_text;
  env_text.reserve(options.env.size());
  for (const auto& [name, value] : options.env) {
    env_text.push_back(name + "=" + value);
  }
  std::vector<char*> envp;
  for (std::string& entry : env_text) envp.push_back(entry.data());
  envp.push_back(nullptr);

  pid_t pid = -1;
  const int rc = ::posix_spawn(&pid, options.command.c_str(), &actions,
                               &attributes, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);

  ::close(in[0]);
  ::close(stdout_pipe[1]);
  ::close(stderr_pipe[1]);
  if (rc != 0) {
    ::close(in[1]);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);
    error = "could not start " + options.command + ": " + std::strerror(rc);
    return false;
  }

  out.pid = pid;
  out.stdin_fd = in[1];
  out.stdout_fd = stdout_pipe[0];
  out.stderr_fd = stderr_pipe[0];
  set_nonblocking(out.stdin_fd);
  set_nonblocking(out.stdout_fd);
  set_nonblocking(out.stderr_fd);
  set_nosigpipe(out.stdin_fd);
  return true;
}

bool reap(pid_t pid, int& status) {
  if (pid <= 0) return true;
  const pid_t result = ::waitpid(pid, &status, WNOHANG);
  if (result == pid) return true;
  if (result < 0 and errno == ECHILD) {
    status = 0;
    return true;
  }
  return false;
}

std::string describe_exit(int status) {
  if (WIFEXITED(status)) {
    return "exited with status " + std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    const int number = WTERMSIG(status);
    const char* name = ::strsignal(number);
    return "killed by signal " + std::to_string(number) +
           (name != nullptr ? " (" + std::string(name) + ")" : std::string());
  }
  return "ended";
}

}  // namespace util
