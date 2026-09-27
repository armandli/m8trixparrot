#ifndef M8_PATHS_H
#define M8_PATHS_H

#include <string>
#include <vector>

namespace m8 {

// The nearest ancestor of `cwd` (inclusive) that carries a workspace marker —
// .git or .m8. Empty when `cwd` sits under neither, which is the signal that m8
// was started outside a project and should keep its state in the cwd rather than
// wander up into the user's home directory looking for somewhere to write.
//
// Taking the starting directory as an argument rather than calling
// current_path() is what lets a test drive it without chdir'ing.
std::string find_workspace_root(const std::string& cwd);

// Everything m8 owns, one place per purpose, all of it inside the repository it
// is working on. That is the point: two checkouts of the same project get two
// independent sets of skills, sessions and memory, and none of it follows the
// user home.
//
// Deliberately NOT here: the bash_search index. It is a description of PATH,
// which is a property of the machine rather than of this repository, so it stays
// shared at ~/.m8/bash_search_index.json — see home_dir()/search_index().
struct M8Paths {
  // The workspace root, or the cwd when there is no marker above it.
  std::string root;
  // $HOME, or empty when the environment has none.
  std::string home;

  std::string dir() const { return root + "/.m8"; }
  std::string config_file() const { return dir() + "/config.json"; }
  std::string skills() const { return dir() + "/skills"; }
  std::string vdb() const { return dir() + "/vdb"; }
  std::string memory() const { return vdb() + "/memory.m8db"; }
  std::string sessions() const { return dir() + "/sessions"; }
  std::string api_key() const { return dir() + "/parallel_api_key"; }

  // ~/.m8, and the PATH index inside it. Empty when `home` is, in which case
  // the caller leaves the bash_search default alone.
  std::string home_dir() const { return home.empty() ? "" : home + "/.m8"; }
  std::string search_index() const {
    return home.empty() ? "" : home_dir() + "/bash_search_index.json";
  }

  // False when `root` carries no marker: m8 is running in a bare directory. Its
  // state still lands in ./.m8, but the prompt says so, because a scratch
  // directory is not the repository the user probably meant.
  bool in_workspace = false;
};

M8Paths resolve_m8_paths(const std::string& cwd, const std::string& home);

// Creates .m8/{skills,vdb,sessions} and ~/.m8 up front, so neither m8 nor the
// model ever has to mkdir a directory something is about to write to. A failure
// is reported rather than fatal: m8 still runs, it just cannot persist.
bool ensure_m8_dirs(const M8Paths& paths, std::string& error);

// Writes a commented default config.json when none exists, so the first thing a
// user sees is the set of knobs rather than an empty directory. Returns false
// only when the file was meant to be written and could not be; an existing file
// is left exactly as it is and returns true.
bool ensure_default_config(const M8Paths& paths, std::string& error);

// Which of `names` can be found on `path_env` as a file this user can actually
// execute. m8 reaches its file and web tools by running them in the shell, so
// "is tool_grep installed" is a question it has to be able to answer at startup
// — both to warn the user and to keep the system prompt from advertising a
// command that is not there, or is there but unrunnable.
std::vector<std::string> find_on_path(const std::vector<std::string>& names,
                                      const std::string& path_env);

}  // namespace m8

#endif  // M8_PATHS_H
