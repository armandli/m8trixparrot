#include <core/skills.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

#include <core/tools_util.h>

namespace agent {

namespace {

std::string trim(std::string_view s) {
  const auto not_space = [](unsigned char c) { return not std::isspace(c); };
  const auto begin = std::find_if(s.begin(), s.end(), not_space);
  const auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
  return begin < end ? std::string(begin, end) : std::string();
}

std::string unquote(std::string value) {
  if (value.size() >= 2 and
      ((value.front() == '"' and value.back() == '"') or
       (value.front() == '\'' and value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string token;
  while (in >> token) out.push_back(token);
  return out;
}

bool truthy(const std::string& v) {
  return v == "true" or v == "yes" or v == "1";
}

bool falsy(const std::string& v) {
  return v == "false" or v == "no" or v == "0";
}

// Splits a "key: value" line. Returns false if there is no colon.
bool split_kv(const std::string& line, std::string& key, std::string& value) {
  const size_t colon = line.find(':');
  if (colon == std::string::npos) return false;
  key = trim(line.substr(0, colon));
  value = unquote(trim(line.substr(colon + 1)));
  return true;
}

}  // namespace

SkillFrontmatter parse_frontmatter(const std::string& text) {
  SkillFrontmatter fm;

  std::istringstream in(text);
  std::string line;
  if (not std::getline(in, line)) return fm;
  if (not line.empty() and line.back() == '\r') line.pop_back();
  if (line != "---") return fm;

  bool closed = false;
  bool in_metadata = false;
  while (std::getline(in, line)) {
    if (not line.empty() and line.back() == '\r') line.pop_back();
    if (line == "---") {
      closed = true;
      break;
    }

    const bool indented =
        not line.empty() and (line[0] == ' ' or line[0] == '\t');

    if (in_metadata) {
      if (indented) {
        std::string key, value;
        if (split_kv(line, key, value) and not key.empty()) {
          fm.metadata[key] = value;
        }
        continue;
      }
      in_metadata = false;  // dedented: back to top-level keys
    }

    const std::string trimmed = trim(line);
    if (trimmed.empty() or trimmed[0] == '#') continue;

    std::string key, value;
    if (not split_kv(line, key, value)) continue;
    if (key == "metadata") {
      in_metadata = true;
    } else if (key == "name") {
      fm.name = value;
    } else if (key == "description") {
      fm.description = value;
    }
    // Every other key (license, compatibility, allowed-tools, version, ...) is
    // ignored, as the spec's lenient loading allows.
  }

  fm.ok = closed and not fm.description.empty();
  return fm;
}

SkillCatalog SkillCatalog::discover(const std::string& skills_dir) {
  SkillCatalog catalog;
  if (skills_dir.empty()) return catalog;

  std::error_code ec;
  if (not std::filesystem::is_directory(skills_dir, ec)) return catalog;

  std::vector<std::filesystem::path> dirs;
  for (const auto& entry :
       std::filesystem::directory_iterator(skills_dir, ec)) {
    if (entry.is_directory(ec)) dirs.push_back(entry.path());
  }
  std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
    return a.filename().string() < b.filename().string();
  });

  for (const std::filesystem::path& dir : dirs) {
    const std::string dir_name = dir.filename().string();
    const std::filesystem::path skill_md = dir / "SKILL.md";
    if (not std::filesystem::is_regular_file(skill_md, ec)) continue;

    const std::optional<std::string> text = read_file(skill_md.string());
    if (not text) {
      catalog.notes.push_back("skipped '" + dir_name + "': cannot read SKILL.md");
      continue;
    }

    const SkillFrontmatter fm = parse_frontmatter(*text);
    if (not fm.ok) {
      catalog.notes.push_back(
          "skipped '" + dir_name + "': SKILL.md has no frontmatter description");
      continue;
    }

    SkillInfo info;
    info.name = dir_name;
    if (not fm.name.empty() and fm.name != dir_name) {
      catalog.notes.push_back("'" + dir_name + "': frontmatter name '" +
                              fm.name + "' does not match the directory");
    }
    info.description = fm.description;
    info.dir = dir.string();
    info.skill_md_path = skill_md.string();

    if (const auto it = fm.metadata.find("requires"); it != fm.metadata.end()) {
      info.dependencies = split_ws(it->second);
    }
    if (const auto it = fm.metadata.find("command"); it != fm.metadata.end()) {
      info.command = truthy(it->second);
    }
    if (const auto it = fm.metadata.find("argument-hint");
        it != fm.metadata.end()) {
      info.argument_hint = it->second;
    }
    if (const auto it = fm.metadata.find("model-invocable");
        it != fm.metadata.end()) {
      info.model_invocable = not falsy(it->second);
    }

    catalog.skills.push_back(std::move(info));
  }

  return catalog;
}

const SkillInfo* SkillCatalog::find(std::string_view name) const {
  for (const SkillInfo& skill : skills) {
    if (skill.name == name) return &skill;
  }
  return nullptr;
}

std::string SkillCatalog::label_for_text(std::string_view text) const {
  for (const SkillInfo& skill : skills) {
    if (not skill.dir.empty() and
        text.find(skill.dir) != std::string_view::npos) {
      return skill.name;
    }
  }
  return std::string();
}

int64_t estimate_transcript_tokens(const std::vector<ChatMessage>& transcript) {
  size_t chars = 0;
  for (const ChatMessage& message : transcript) {
    chars += message.content.size();
    for (const ToolCall& call : message.tool_calls) {
      chars += call.name.size() + call.arguments.size();
    }
  }
  return static_cast<int64_t>(chars / 4) + 1200;
}

// ---------------------------------------------------------------------------
// SkillTool
// ---------------------------------------------------------------------------

std::string SkillTool::description() {
  return R"json({"name":"skill","description":"Load a skill's instructions into the conversation, or unload them to reclaim context. Skills are reusable procedures for specific tasks, listed in the system prompt. action=\"load\" reads .m8trix/skills/<name>/SKILL.md (or a file relative to the skill directory when `file` is given); read the skill's other files with `python`. action=\"unload\" removes everything you loaded for that skill from the conversation.","parameters":{"type":"object","properties":{"action":{"type":"string","enum":["load","unload"],"description":"load or unload"},"name":{"type":"string","description":"the skill name (its directory under .m8trix/skills/)"},"file":{"type":"string","description":"load only: a path relative to the skill directory, e.g. references/patterns.md; omit for SKILL.md"}},"required":["action","name"]}})json";
}

ToolResult SkillTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> action = string_arg(args, "action");
  const std::optional<std::string> name = string_arg(args, "name");
  if (not action or not name or name->empty()) {
    result.error =
        "skill: requires string arguments 'action' (load|unload) and 'name'";
    return result;
  }

  const SkillInfo* skill = catalog.find(*name);
  if (not skill) {
    std::string available;
    for (const SkillInfo& s : catalog.skills) {
      available += (available.empty() ? " (available: " : ", ") + s.name;
    }
    if (not available.empty()) available += ")";
    result.error = "skill: no skill named '" + *name + "'" + available;
    return result;
  }

  if (*action == "load") return load(*skill, string_arg(args, "file"));
  if (*action == "unload") return unload(*skill);

  result.error =
      "skill: action must be 'load' or 'unload', not '" + *action + "'";
  return result;
}

ToolResult SkillTool::load(const SkillInfo& skill,
                           const std::optional<std::string>& file) const {
  ToolResult result;

  std::string relative = file.value_or("SKILL.md");
  if (relative.empty()) relative = "SKILL.md";
  if (relative.front() == '/' or relative.front() == '\\' or
      relative.find("..") != std::string::npos) {
    result.error =
        "skill: 'file' must be a relative path inside the skill directory";
    return result;
  }

  const std::filesystem::path target =
      std::filesystem::path(skill.dir) / relative;
  const std::optional<std::string> text = read_file(target.string());
  if (not text) {
    result.error = "skill: cannot read '" + relative + "' in skill '" +
                   skill.name + "'";
    return result;
  }

  std::string out = *text;
  if (relative == "SKILL.md") {
    out += "\n\n---\n[skill \"" + skill.name +
           "\" loaded. Directory: " + skill.dir + "/.";
    if (not skill.dependencies.empty()) {
      out += " Depends on:";
      for (const std::string& dep : skill.dependencies) out += " " + dep;
      out += " (load those separately if you need them).";
    }
    out += " Read its other files with `python` or `skill` action \"load\" with "
           "`file`. Call `skill` action \"unload\" name=\"" +
           skill.name + "\" when finished to reclaim context.]";
  }

  result.ok = true;
  result.output = std::move(out);
  return result;
}

ToolResult SkillTool::unload(const SkillInfo& skill) const {
  ToolResult result;

  size_t removed = 0;
  size_t freed = 0;
  // Back-to-front so erasing keeps the remaining indices valid.
  for (long i = static_cast<long>(transcript.size()) - 1; i >= 0; --i) {
    if (transcript[static_cast<size_t>(i)].skill_label != skill.name) continue;

    const ChatMessage& msg = transcript[static_cast<size_t>(i)];
    freed += msg.content.size();

    if (msg.role != "tool") {
      transcript.erase(transcript.begin() + i);
      ++removed;
      continue;
    }

    // A tool result: find the assistant message that owns it, skipping any
    // sibling tool results in between. The loop always emits N tool messages in
    // order right after an assistant carrying N tool_calls.
    long j = i - 1;
    while (j >= 0 and transcript[static_cast<size_t>(j)].role == "tool") --j;

    if (j >= 0 and transcript[static_cast<size_t>(j)].role == "assistant" and
        not transcript[static_cast<size_t>(j)].tool_calls.empty()) {
      ChatMessage& owner = transcript[static_cast<size_t>(j)];
      const size_t sibling = static_cast<size_t>(i - (j + 1));
      if (sibling < owner.tool_calls.size()) {
        owner.tool_calls.erase(owner.tool_calls.begin() + sibling);
      }
      transcript.erase(transcript.begin() + i);
      ++removed;
      if (owner.tool_calls.empty() and owner.content.empty()) {
        transcript.erase(transcript.begin() + j);
        ++removed;
      }
    } else {
      transcript.erase(transcript.begin() + i);
      ++removed;
    }
  }

  if (removed == 0) {
    result.ok = true;
    result.output = "skill '" + skill.name + "' is not loaded";
    return result;
  }

  context_tokens.store(estimate_transcript_tokens(transcript));
  result.ok = true;
  result.output = "unloaded skill '" + skill.name + "': removed " +
                  std::to_string(removed) + " message(s), ~" +
                  std::to_string(freed) + " chars reclaimed";
  return result;
}

}  // namespace agent
