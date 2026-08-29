#ifndef AGENT_SETTINGS_H
#define AGENT_SETTINGS_H

#include <optional>
#include <string>

namespace agent {

// Where the startup settings file lives, relative to the working directory.
// Read once at process startup (see load_startup_settings); nothing in the
// agent core re-reads it during a run.
inline constexpr const char* kAgentSettingsPath = ".m8trix/settings.json";

// The subset of startup configuration that can be pinned per-repo via
// kAgentSettingsPath: the model and permission policy, plus every field of
// AgentOptions. Every field is optional so a caller can layer "unset -> keep
// whatever default was already in force" without this struct knowing what
// those defaults are; a command line flag applied after loading these
// settings should still win.
struct StartupSettings {
  std::optional<std::string> model;
  std::optional<std::string> policy;
  std::optional<int> max_steps;
  std::optional<int> max_depth;
  std::optional<int> max_agents;
  std::optional<int> num_ctx;
  std::optional<int> summarize_at;
  std::optional<std::string> skills_dir;
  std::optional<bool> enable_skills;
  std::optional<bool> enable_subagents;
  std::optional<bool> enable_package_install;

  // m8trixsh only; the other apps ignore these.
  std::optional<std::string> shell;             // the shell to run in the PTY pane
  std::optional<std::string> mode_switch_key;   // key that toggles shell/ai mode
};

// Reads `path` as a StartupSettings if it exists. A missing file is the
// normal case (no settings file has been created yet): returns a
// StartupSettings with every field unset and leaves `warning` untouched. A
// file that exists but can't be read, isn't valid JSON, or whose top level
// isn't a JSON object is reported through `warning` (also returning an
// all-unset StartupSettings) rather than aborting the caller. A key that is
// absent or holds an unexpected type is left unset rather than erroring —
// same fallback philosophy as json_util's *_field() readers.
StartupSettings load_startup_settings(const std::string& path,
                                      std::string& warning);

}  // namespace agent

#endif  // AGENT_SETTINGS_H
