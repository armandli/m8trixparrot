#ifndef SP_PATHS_H
#define SP_PATHS_H

#include <string>

namespace sp {

// The four XDG base directories, read from the environment by main() and
// passed in, so resolving sp's paths stays a pure function of its inputs and a
// test needs neither a real environment nor a real home.
//
// An entry that is empty or relative is ignored, as the spec requires: only an
// absolute path overrides the default.
struct XdgEnv {
  std::string data_home;    // $XDG_DATA_HOME   default ~/.local/share
  std::string config_home;  // $XDG_CONFIG_HOME default ~/.config
  std::string state_home;   // $XDG_STATE_HOME  default ~/.local/state
  std::string cache_home;   // $XDG_CACHE_HOME  default ~/.cache
};

// Everything sp owns, one place per purpose, instead of the six unrelated
// locations it used to scatter files across — including session files, which
// landed in whatever directory the user happened to be standing in.
struct SpPaths {
  std::string data;    // <data_home>/sp
  std::string config;  // <config_home>/sp
  std::string state;   // <state_home>/sp
  std::string cache;   // <cache_home>/sp

  std::string bin() const { return data + "/bin"; }
  std::string src() const { return data + "/src"; }
  std::string trash() const { return data + "/trash"; }
  std::string memory() const { return data + "/memory.m8db"; }
  std::string config_file() const { return config + "/config"; }
  std::string sessions() const { return state + "/sessions"; }
  std::string search_index() const {
    return cache + "/bash_search_index.json";
  }

  // Whether bin() is an entry in the PATH sp was started with. Commands sp
  // installs are only runnable by name when it is, so this decides whether sp
  // tells the user to add it — which, unlike the old ~/.local/bin, it usually
  // has to.
  bool bin_on_path = false;
};

SpPaths resolve_sp_paths(const std::string& home, const XdgEnv& xdg,
                         const std::string& path_env);

// Creates data, bin, src and trash up front, so the model never has to mkdir a
// directory the prompt told it to write to. A failure is reported rather than
// fatal: sp still runs, it just cannot install anything.
bool ensure_sp_dirs(const SpPaths& paths, std::string& error);

// Moves what sp left in its old locations into the new layout, once:
//
//   ~/.local/sp_development/*  ->  <data>/src/
//   ~/.sprc                    ->  <config>/config
//
// The memory database needs no move — the new data root is the directory it
// was already in. Each move appends a line to `notes` for the caller to print,
// so a user is told what happened to their files rather than finding them
// gone. A second run finds nothing and says nothing.
//
// Deliberately NOT moved, and reported instead: commands already installed in
// ~/.local/bin or ~/bin. sp keeps no record of which files there are its own,
// and those directories hold the user's binaries too — guessing risks moving
// something sp never wrote. They keep working where they are.
void migrate_legacy_paths(const std::string& home, const SpPaths& paths,
                          std::string& notes);

}  // namespace sp

#endif  // SP_PATHS_H
