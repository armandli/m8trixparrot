#ifndef SKILLS_H
#define SKILLS_H

#include <atomic>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/basic_ollama_client.h>
#include <core/tools.h>

namespace agent {

// A skill is a directory <skills_dir>/<name>/ with a SKILL.md root file, in the
// Agent Skills format (https://agentskills.io): YAML-ish frontmatter between
// `---` lines, then a markdown body. The agent sees only name + description in
// its system prompt (the "catalog") and loads the body on demand with the
// `skill` tool; supporting files under the skill directory are read with
// `python`.

struct SkillInfo {
  std::string name;         // the directory name; frontmatter `name` must match
  std::string description;  // frontmatter `description`
  std::string dir;             // "<skills_dir>/<name>"
  std::string skill_md_path;   // "<dir>/SKILL.md"

  // From the frontmatter's `metadata:` map. These are m8trix extensions the
  // Agent Skills spec allows there (its top-level keys stay portable).
  std::vector<std::string> dependencies;  // metadata.requires; informational only
  bool command = false;                    // metadata.command: /<name> in the TUI
  std::string argument_hint;               // metadata.argument-hint
  bool model_invocable = true;              // metadata.model-invocable != "false"
};

// Result of parsing a SKILL.md's frontmatter. Exposed for testing.
struct SkillFrontmatter {
  std::string name;
  std::string description;
  std::map<std::string, std::string> metadata;  // one level deep, values as text

  // False when there is no `---`-fenced block or the description is empty; the
  // spec says to skip such a skill.
  bool ok = false;
};

// Parses the leading `---` ... `---` frontmatter. Lenient (per the spec): unknown
// keys are ignored, block scalars / nested maps beyond `metadata:` are skipped.
SkillFrontmatter parse_frontmatter(const std::string& skill_md_text);

struct SkillCatalog {
  std::vector<SkillInfo> skills;   // sorted by name
  std::vector<std::string> notes;  // skipped-skill reasons, name/dir mismatches

  // Scans <skills_dir> for subdirectories containing a SKILL.md. An empty or
  // missing directory yields an empty catalog.
  static SkillCatalog discover(const std::string& skills_dir);

  const SkillInfo* find(std::string_view name) const;

  // Name of the first skill whose directory path appears in `text` (a python
  // script or its argument JSON), or "" — used to tag transcript messages that
  // read a skill's files so `skill unload` can find them.
  std::string label_for_text(std::string_view text) const;
};

// The `chars / 4 + 1200` transcript-size estimate shared by the summarize
// trigger and `skill unload`'s post-removal token refresh.
int64_t estimate_transcript_tokens(const std::vector<ChatMessage>& transcript);

// Loads a skill's instructions into the transcript, or removes them. Constructed
// at the dispatch site with references into the running Agent, like the subagent
// tools.
struct SkillTool {
  std::vector<ChatMessage>& transcript;
  std::atomic<int64_t>& context_tokens;
  const SkillCatalog& catalog;

  static std::string description();
  ToolResult execute(const ToolArgs& args) const;

 protected:
  ToolResult load(const SkillInfo& skill,
                  const std::optional<std::string>& file) const;
  ToolResult unload(const SkillInfo& skill) const;
};

}  // namespace agent

#endif  // SKILLS_H
