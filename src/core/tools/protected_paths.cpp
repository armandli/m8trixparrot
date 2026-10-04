#include <core/tools/protected_paths.h>

#include <cstddef>
#include <cstdlib>

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

namespace tools {

namespace {

// How an entry's pattern is compared against a resolved path.
enum struct Match : int {
  // The path itself, or anything beneath it. Compared component-wise, so
  // ~/.ssh does not admit ~/.sshfoo.
  Prefix,
  // Exactly this path and nothing else.
  Exact,
  // A path with this component anywhere in it, at any depth. For `.git`, which
  // is a directory that exists once per repository rather than in one place.
  AnyComponent,
  // A path whose last components are exactly these, wherever it lives. For
  // files every workspace has its own copy of, like `.m8/mcp.json`: an absolute
  // pattern cannot name them, and a component match on `.m8` would take the
  // whole directory, sessions and skills included.
  PathSuffix,
};

struct Entry {
  Match match;
  // Absolute, or starting with "~/" for one under the user's home. Canonicalized
  // once at startup (see entries()), which matters because on macOS /etc is a
  // symlink to /private/etc and a resolved path arrives in the latter form.
  const char* pattern;
  // Secret tier: reading it leaks something, so reads are refused too.
  bool secret;
  // Named in the refusal, so the model is told what kind of file it hit rather
  // than just that it lost.
  const char* reason;
};

// ───────────────────────────── the Secret tier ──────────────────────────────
// Reading any of these hands a credential to whatever is on the other end of the
// transcript, so read is refused alongside write and edit.
constexpr Entry kSecrets[] = {
    {Match::Prefix, "~/.ssh", true, "ssh keys and config"},
    {Match::Prefix, "~/.gnupg", true, "GnuPG keyring"},
    {Match::Prefix, "~/.aws", true, "AWS credentials"},
    {Match::Prefix, "~/.azure", true, "Azure credentials"},
    {Match::Prefix, "~/.kube", true, "Kubernetes credentials"},
    {Match::Prefix, "~/.config/gcloud", true, "Google Cloud credentials"},
    {Match::Prefix, "~/.config/gh", true, "GitHub CLI token"},
    {Match::Exact, "~/.config/hub", true, "GitHub token"},
    {Match::Exact, "~/.docker/config.json", true, "Docker registry credentials"},
    {Match::Prefix, "~/Library/Keychains", true, "macOS keychain"},
    {Match::Exact, "~/.git-credentials", true, "stored git credentials"},
    {Match::Exact, "~/.netrc", true, "netrc credentials"},
    {Match::Exact, "~/.authinfo", true, "authinfo credentials"},
    {Match::Exact, "~/.authinfo.gpg", true, "authinfo credentials"},
    {Match::Exact, "~/.npmrc", true, "npm token"},
    {Match::Exact, "~/.pypirc", true, "PyPI token"},
    {Match::Exact, "~/.cargo/credentials", true, "crates.io token"},
    {Match::Exact, "~/.cargo/credentials.toml", true, "crates.io token"},
    {Match::Exact, "/etc/shadow", true, "system password hashes"},
    {Match::Exact, "/etc/sudoers", true, "sudo configuration"},
    {Match::Prefix, "/etc/sudoers.d", true, "sudo configuration"},
    // This project's own secret, in both of the places a key is kept: the
    // standalone tool_websearch command reads ~/.parallel_api_key, and the
    // websearch tool call reads .m8/parallel_api_key in whichever workspace it
    // runs (which .gitignore already calls out by name).
    {Match::Exact, "~/.parallel_api_key", true, "this project's API key"},
    {Match::PathSuffix, ".m8/parallel_api_key", true, "this project's API key"},
    // MCP. A server entry carries tokens in its env and headers, and both the
    // workspace file and ~/.m8/mcp.json end in this suffix. The credentials
    // file is OAuth bearer and refresh tokens outright.
    {Match::PathSuffix, ".m8/mcp.json", true,
     "MCP server configuration, which can hold tokens"},
    {Match::Exact, "~/.m8/mcp_credentials.json", true, "MCP OAuth credentials"},
};

// ──────────────────────── the ExecutionVector tier ──────────────────────────
// Writing any of these gets something run later — on the next shell, the next
// commit, the next login, the next boot. Reading them is fine.
constexpr Entry kExecutionVectors[] = {
    // Shell startup files. The classic persistence vector: whatever lands here
    // runs on the next interactive shell.
    {Match::Exact, "~/.zshrc", false, "shell startup file"},
    {Match::Exact, "~/.zshenv", false, "shell startup file"},
    {Match::Exact, "~/.zprofile", false, "shell startup file"},
    {Match::Exact, "~/.zlogin", false, "shell startup file"},
    {Match::Exact, "~/.zlogout", false, "shell startup file"},
    {Match::Exact, "~/.bashrc", false, "shell startup file"},
    {Match::Exact, "~/.bash_profile", false, "shell startup file"},
    {Match::Exact, "~/.bash_login", false, "shell startup file"},
    {Match::Exact, "~/.bash_logout", false, "shell startup file"},
    {Match::Exact, "~/.profile", false, "shell startup file"},
    {Match::Exact, "~/.inputrc", false, "shell startup file"},
    {Match::Prefix, "~/.config/fish", false, "shell startup file"},

    // git. core.sshCommand and alias.* are both arbitrary command execution, and
    // a hook runs on the next commit or checkout. An agent that wants to change
    // a repository's git state runs `git`; it has no business writing in here.
    {Match::Exact, "~/.gitconfig", false, "git configuration (can run commands)"},
    {Match::Exact, "~/.config/git/config", false,
     "git configuration (can run commands)"},
    {Match::AnyComponent, ".git", false,
     "a repository's internal git directory (hooks and config run commands)"},

    // MCP server approvals and enable choices. Writing either approves or turns
    // on a server, whose command then runs on the next launch — or at once, on
    // the next `m8 mcp list` or `get`. The state file is per workspace.
    {Match::Exact, "~/.m8/mcp_trust.json", false,
     "MCP server approvals (an approved server's command runs)"},
    {Match::PathSuffix, ".m8/mcp_state.json", false,
     "MCP server enable choices (an enabled server's command runs)"},

    // Login and boot persistence, macOS then Linux.
    {Match::Prefix, "~/Library/LaunchAgents", false, "a launchd agent"},
    {Match::Prefix, "/Library/LaunchAgents", false, "a launchd agent"},
    {Match::Prefix, "/Library/LaunchDaemons", false, "a launchd daemon"},
    {Match::Prefix, "~/.config/systemd/user", false, "a systemd unit"},
    {Match::Prefix, "/etc/systemd", false, "a systemd unit"},
    {Match::Prefix, "~/.config/autostart", false, "an autostart entry"},
    {Match::Exact, "/etc/crontab", false, "a crontab"},
    {Match::Prefix, "/etc/cron.d", false, "a crontab"},
    {Match::Prefix, "/var/spool/cron", false, "a crontab"},
    {Match::Prefix, "/var/at", false, "a scheduled at(1) job"},

    // System directories. /var is deliberately absent: $TMPDIR is under
    // /var/folders on macOS, so blocking it would break every temporary file in
    // the process. The two /var entries above are narrow on purpose.
    {Match::Prefix, "/etc", false, "system configuration"},
    {Match::Prefix, "/usr", false, "a system directory"},
    {Match::Prefix, "/bin", false, "a system directory"},
    {Match::Prefix, "/sbin", false, "a system directory"},
    {Match::Prefix, "/boot", false, "a system directory"},
    {Match::Prefix, "/System", false, "a system directory"},
    {Match::Prefix, "/Library", false, "a system directory"},
    {Match::Prefix, "/dev", false, "a device file"},
};

std::string home_dir() {
  const char* home = std::getenv("HOME");
  if (home == nullptr or *home == '\0' or *home != '/') return std::string();
  std::string text(home);
  while (text.size() > 1 and text.back() == '/') text.pop_back();
  return text;
}

// One resolved entry. The pattern is turned into a real path once, at first use,
// so the per-call work is comparison only.
struct Resolved {
  Match match;
  std::filesystem::path path;       // Prefix and Exact only.
  std::string component;            // AnyComponent only.
  std::vector<std::string> suffix;  // PathSuffix only.
  bool secret;
  const char* reason;
};

std::filesystem::path canonicalize(const std::string& pattern,
                                   const std::string& home) {
  std::string text = pattern;
  if (text.rfind("~/", 0) == 0) {
    // No HOME means no home-relative entry can match anything; an empty path is
    // skipped by the matcher below.
    if (home.empty()) return std::filesystem::path();
    text = home + text.substr(1);
  }

  std::error_code ec;
  std::filesystem::path resolved = std::filesystem::weakly_canonical(text, ec);
  if (ec or resolved.empty()) {
    resolved = std::filesystem::path(text).lexically_normal();
  }
  return resolved;
}

const std::vector<Resolved>& entries() {
  static const std::vector<Resolved> resolved = [] {
    const std::string home = home_dir();
    std::vector<Resolved> out;
    out.reserve(std::size(kSecrets) + std::size(kExecutionVectors));

    const auto add = [&out, &home](const Entry& entry) {
      if (entry.match == Match::AnyComponent) {
        out.push_back(Resolved{entry.match, {}, entry.pattern, {}, entry.secret,
                               entry.reason});
        return;
      }
      if (entry.match == Match::PathSuffix) {
        std::vector<std::string> parts;
        for (const std::filesystem::path& part :
             std::filesystem::path(entry.pattern)) {
          parts.push_back(part.string());
        }
        out.push_back(Resolved{entry.match, {}, {}, std::move(parts),
                               entry.secret, entry.reason});
        return;
      }
      const std::filesystem::path path = canonicalize(entry.pattern, home);
      if (path.empty()) return;  // home-relative with no HOME
      out.push_back(
          Resolved{entry.match, path, {}, {}, entry.secret, entry.reason});
    };

    for (const Entry& entry : kSecrets) add(entry);
    for (const Entry& entry : kExecutionVectors) add(entry);
    return out;
  }();
  return resolved;
}

// Component-wise, so /usr does not admit /usrfoo and ~/.ssh does not admit
// ~/.sshfoo. The same idiom as policy::SanePolicy's containment check; a string
// prefix test here would be a real bug, not a style choice.
bool under(const std::filesystem::path& root,
           const std::filesystem::path& candidate) {
  auto [root_it, candidate_it] = std::mismatch(
      root.begin(), root.end(), candidate.begin(), candidate.end());
  return root_it == root.end();
}

bool has_component(const std::filesystem::path& path,
                   const std::string& component) {
  for (const std::filesystem::path& part : path) {
    if (part.string() == component) return true;
  }
  return false;
}

bool ends_with(const std::filesystem::path& path,
               const std::vector<std::string>& suffix) {
  std::vector<std::string> parts;
  for (const std::filesystem::path& part : path) parts.push_back(part.string());
  if (suffix.empty() or parts.size() < suffix.size()) return false;
  return std::equal(suffix.begin(), suffix.end(),
                    parts.end() - static_cast<std::ptrdiff_t>(suffix.size()));
}

// The matching entry for `path`, or null.
const Resolved* find_entry(const std::filesystem::path& resolved) {
  for (const Resolved& entry : entries()) {
    switch (entry.match) {
      case Match::Prefix:
        if (under(entry.path, resolved)) return &entry;
        break;
      case Match::Exact:
        if (entry.path == resolved) return &entry;
        break;
      case Match::AnyComponent:
        if (has_component(resolved, entry.component)) return &entry;
        break;
      case Match::PathSuffix:
        if (ends_with(resolved, entry.suffix)) return &entry;
        break;
    }
  }
  return nullptr;
}

}  // namespace

bool is_pseudo_device(const std::string& path) {
  return path == "/dev/null" or path == "/dev/stdout" or
         path == "/dev/stderr" or path == "/dev/tty" or path == "/dev/zero" or
         path.rfind("/dev/fd/", 0) == 0;
}

std::string expand_home(const std::string& path) {
  // bash expands an unquoted ~ before the tool is even started, but a quoted one
  // arrives verbatim — and std::filesystem treats "~/.ssh/x" as a RELATIVE path,
  // so without this it would resolve under the current directory and match
  // nothing. $HOME is spelled out for the same reason: it is just as easy for a
  // model to write and just as invisible to a path check.
  const std::string home = home_dir();
  if (home.empty()) return path;

  if (path == "~" or path == "$HOME") return home;
  if (path.rfind("~/", 0) == 0) return home + path.substr(1);
  if (path.rfind("$HOME/", 0) == 0) return home + path.substr(5);
  return path;
}

std::filesystem::path resolve_for_guard(const std::string& path) {
  const std::string text = expand_home(path);

  std::error_code ec;
  std::filesystem::path target(text);
  if (target.is_relative()) {
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (not ec and not cwd.empty()) target = cwd / target;
  }

  // Normalizes `..` and follows symlinks on the part that exists, which is what
  // catches a link standing in for a protected file or for its parent
  // directory. canonical() would fail outright on a file yet to be created.
  std::filesystem::path resolved = std::filesystem::weakly_canonical(target, ec);
  if (ec or resolved.empty()) resolved = target.lexically_normal();
  return resolved;
}

bool is_protected_secret(const std::string& path) {
  if (path.empty()) return false;
  if (is_pseudo_device(path)) return false;
  const Resolved* entry = find_entry(resolve_for_guard(path));
  return entry != nullptr and entry->secret;
}

std::string protected_path_reason(const std::string& path, PathAccess access,
                                  std::string_view tool_name) {
  // An empty path is the tool's own argument error to report, with its own
  // wording; saying anything here would only obscure it.
  if (path.empty()) return std::string();
  if (is_pseudo_device(path)) return std::string();

  const std::filesystem::path resolved = resolve_for_guard(path);
  const Resolved* entry = find_entry(resolved);
  if (entry == nullptr) return std::string();
  // An ExecutionVector is dangerous to write, not to read.
  if (access == PathAccess::Read and not entry->secret) return std::string();

  const bool reading = access == PathAccess::Read;
  std::string message = resolved.string();
  message += " is a protected path (";
  message += entry->reason;
  message += ") and cannot be ";
  message += reading ? "read" : "written";
  message += " with tool_";
  message += tool_name;
  message += ". This is not a permissions problem and there is no override — do "
             "not try another way to ";
  message += reading ? "read it." : "write it.";
  message += reading
                 ? " Ask the user for what you need from it."
                 : " Tell the user what change you wanted to make and let them "
                   "make it.";
  return message;
}

}  // namespace tools
