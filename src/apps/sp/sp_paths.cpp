#include <filesystem>
#include <system_error>
#include <vector>

#include <sp_paths.h>

namespace sp {

namespace {

// "/home/ada/.local/share/sp/bin/" and ".../bin" name the same directory, and
// PATH entries are written both ways.
std::string strip_slash(std::string path) {
  while (path.size() > 1 and path.back() == '/') path.pop_back();
  return path;
}

// Split on ':', the one separator PATH uses. An empty entry means the current
// directory by POSIX convention, which is never one of our candidates, so
// dropping it costs nothing.
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

bool listed_in_path(const std::string& dir,
                    const std::vector<std::string>& entries) {
  const std::string wanted = strip_slash(dir);
  for (const std::string& entry : entries) {
    if (entry == wanted) return true;
  }
  return false;
}

// The spec is explicit that a relative $XDG_*_HOME must be ignored rather than
// resolved against anything — and an empty one (XDG_DATA_HOME= in a shell rc)
// is the common way it shows up.
std::string xdg_or(const std::string& value, const std::string& fallback) {
  if (value.empty() or value.front() != '/') return fallback;
  return strip_slash(value);
}

bool move_path(const std::filesystem::path& from,
               const std::filesystem::path& to, std::string& notes) {
  std::error_code ec;
  if (not std::filesystem::exists(from, ec)) return false;

  std::filesystem::create_directories(to.parent_path(), ec);
  std::filesystem::rename(from, to, ec);
  if (ec) {
    // Across filesystems rename fails where a copy would work.
    std::error_code copy_ec;
    std::filesystem::copy(from, to,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::skip_existing,
                          copy_ec);
    if (copy_ec) {
      notes += "warning: could not move " + from.string() + " to " +
               to.string() + ": " + copy_ec.message() + "\n";
      return false;
    }
    std::filesystem::remove_all(from, copy_ec);
  }

  notes += "note: moved " + from.string() + " to " + to.string() + "\n";
  return true;
}

}  // namespace

SpPaths resolve_sp_paths(const std::string& home, const XdgEnv& xdg,
                         const std::string& path_env) {
  SpPaths paths;
  paths.data = xdg_or(xdg.data_home, home + "/.local/share") + "/sp";
  paths.config = xdg_or(xdg.config_home, home + "/.config") + "/sp";
  paths.state = xdg_or(xdg.state_home, home + "/.local/state") + "/sp";
  paths.cache = xdg_or(xdg.cache_home, home + "/.cache") + "/sp";

  paths.bin_on_path = listed_in_path(paths.bin(), path_entries(path_env));
  return paths;
}

bool ensure_sp_dirs(const SpPaths& paths, std::string& error) {
  for (const std::string& dir :
       {paths.data, paths.bin(), paths.src(), paths.trash()}) {
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

void migrate_legacy_paths(const std::string& home, const SpPaths& paths,
                          std::string& notes) {
  namespace fs = std::filesystem;
  std::error_code ec;

  // C++ and script sources sp used to keep in its own top-level directory.
  const fs::path old_dev = fs::path(home) / ".local" / "sp_development";
  if (fs::is_directory(old_dev, ec)) {
    bool moved_any = false;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(old_dev, ec)) {
      const fs::path target = fs::path(paths.src()) / entry.path().filename();
      if (fs::exists(target, ec)) continue;  // already migrated
      moved_any |= move_path(entry.path(), target, notes);
    }
    // Only if it is now empty: never remove a directory still holding
    // something we could not move.
    if (moved_any and fs::is_empty(old_dev, ec)) fs::remove(old_dev, ec);
  }

  // The config dotfile.
  const fs::path old_rc = fs::path(home) / ".sprc";
  const fs::path new_rc = paths.config_file();
  if (fs::exists(old_rc, ec) and not fs::exists(new_rc, ec)) {
    move_path(old_rc, new_rc, notes);
  }

  // Commands sp installed before the move stay where they are: there is no
  // record of which files in a shared bin directory sp wrote, and moving one
  // it did not write would be worse than leaving them all.
  //
  // Only worth saying on the run that actually migrates something. The note is
  // context for a move the user just saw, not an action for them to take, so
  // repeating it on every later run would be nagging about nothing.
  if (notes.empty()) return;
  for (const std::string& legacy : {home + "/.local/bin", home + "/bin"}) {
    if (not fs::is_directory(legacy, ec)) continue;
    if (fs::is_empty(legacy, ec)) continue;
    notes += "note: commands installed before this version are still in " +
             legacy + " and keep working; new ones go to " + paths.bin() +
             "\n";
    break;
  }
}

}  // namespace sp
