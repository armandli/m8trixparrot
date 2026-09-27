#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <system_error>

#include <m8_paths.h>

namespace m8 {

namespace {

// "/usr/local/bin/" and "/usr/local/bin" name the same directory, and PATH
// entries are written both ways.
std::string strip_slash(std::string path) {
  while (path.size() > 1 and path.back() == '/') path.pop_back();
  return path;
}

// Split on ':', the one separator PATH uses. An empty entry means the current
// directory by POSIX convention; honouring that would let a file named tool_grep
// in the cwd pass for the installed command, so it is dropped.
std::vector<std::string> path_entries(const std::string& path_env) {
  std::vector<std::string> entries;
  size_t begin = 0;
  while (begin <= path_env.size()) {
    const size_t colon = path_env.find(':', begin);
    const size_t end = colon == std::string::npos ? path_env.size() : colon;
    if (end > begin) {
      entries.push_back(strip_slash(path_env.substr(begin, end - begin)));
    }
    if (colon == std::string::npos) break;
    begin = colon + 1;
  }
  return entries;
}

bool has_workspace_marker(const std::filesystem::path& dir) {
  namespace sf = std::filesystem;
  std::error_code ec;
  return sf::exists(dir / ".git", ec) or sf::exists(dir / ".m8", ec);
}

// The default config, written once. Every value is the compiled-in default
// spelled out, so the file doubles as the documentation of what can be changed —
// an empty {} would be correct and useless.
constexpr const char* kDefaultConfig = R"json({
  "model": "qwen3.8:27b-mlx",
  "policy": "sane",

  "max_steps": 12,
  "max_depth": 3,
  "max_agents": 16,

  "num_ctx": 0,
  "summarize_at": 200000,
  "ollama_jobs": 2,

  "enable_skills": true,
  "enable_subagents": true,
  "enable_bash_repl": true,
  "enable_bash_search": true,

  "enable_memory": true,
  "memory_embed_model": "nomic-embed-text-v2-moe:latest",

  "enable_web_search": false
}
)json";

}  // namespace

std::string find_workspace_root(const std::string& cwd) {
  namespace sf = std::filesystem;
  std::error_code ec;
  sf::path dir = sf::absolute(sf::path(cwd), ec);
  if (ec) return std::string();
  for (;;) {
    if (has_workspace_marker(dir)) return dir.string();
    const sf::path parent = dir.parent_path();
    if (parent.empty() or parent == dir) return std::string();
    dir = parent;
  }
}

M8Paths resolve_m8_paths(const std::string& cwd, const std::string& home) {
  M8Paths paths;
  paths.home = home.empty() or home.front() != '/' ? "" : strip_slash(home);

  const std::string root = find_workspace_root(cwd);
  paths.in_workspace = not root.empty();
  paths.root = paths.in_workspace ? root : strip_slash(cwd);
  return paths;
}

bool ensure_m8_dirs(const M8Paths& paths, std::string& error) {
  std::vector<std::string> wanted{paths.dir(), paths.skills(), paths.vdb(),
                                  paths.sessions()};
  // Only when there is a home to put it in; a process with no HOME keeps the
  // bash_search default, which falls back to /tmp on its own.
  if (not paths.home_dir().empty()) wanted.push_back(paths.home_dir());

  for (const std::string& dir : wanted) {
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) continue;

    std::filesystem::create_directories(dir, ec);
    if (ec) {
      error = "could not create " + dir + ": " + ec.message();
      return false;
    }
  }
  return true;
}

bool ensure_default_config(const M8Paths& paths, std::string& error) {
  const std::string path = paths.config_file();
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) return true;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (not out) {
    error = "could not write " + path;
    return false;
  }
  out << kDefaultConfig;
  out.close();
  if (not out) {
    error = "could not write " + path;
    return false;
  }
  return true;
}

std::vector<std::string> find_on_path(const std::vector<std::string>& names,
                                      const std::string& path_env) {
  namespace sf = std::filesystem;
  const std::vector<std::string> dirs = path_entries(path_env);

  std::vector<std::string> found;
  for (const std::string& name : names) {
    for (const std::string& dir : dirs) {
      std::error_code ec;
      const sf::path candidate = sf::path(dir) / name;
      if (not sf::is_regular_file(candidate, ec)) continue;
      // A file of the right name this user cannot run is not the command:
      // reporting it installed would advertise a tool that fails.
      //
      // Whether *this* process can execute it is a different question from
      // whether some exec bit is set somewhere — a root-owned 0700 binary has
      // owner_exec and is still unrunnable here. access() is the only check that
      // accounts for uid, supplementary groups, ACLs and a noexec mount, and it
      // costs no more than the second status() it replaces. Plain access() and
      // not eaccess(): the two differ only for a setuid process, m8 is not one,
      // and resolve_shell() in shell_session.cpp already reads this way.
      // Advisory rather than a gate — nothing is executed off the back of it, so
      // the usual access()-then-exec race does not apply.
      if (::access(candidate.c_str(), X_OK) != 0) continue;
      found.push_back(name);
      break;
    }
  }
  return found;
}

}  // namespace m8
