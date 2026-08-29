#include <core/agent_settings.h>

#include <cctype>
#include <cstddef>
#include <cstdlib>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <simdjson.h>

#include <core/tools_util.h>

namespace agent {

namespace {

// Same fallback-on-mismatch idiom as json_util's *_field() readers, but
// returning nullopt (rather than a caller-supplied fallback) so absent-or-
// wrong-type is distinguishable from "present and this value".
std::optional<std::string> optional_string_field(simdjson::ondemand::object& obj,
                                                 std::string_view key) {
  std::string_view out;
  if (obj[key].get_string().get(out)) return std::nullopt;
  return std::string(out);
}

std::optional<int> optional_int_field(simdjson::ondemand::object& obj,
                                      std::string_view key) {
  int64_t out = 0;
  if (obj[key].get_int64().get(out)) return std::nullopt;
  return static_cast<int>(out);
}

std::optional<bool> optional_bool_field(simdjson::ondemand::object& obj,
                                        std::string_view key) {
  bool out = false;
  if (obj[key].get_bool().get(out)) return std::nullopt;
  return out;
}

// ---------------------------------------------------------------------------
// Shell-env-style config (m8trixsh's ~/.m8shrc).
// ---------------------------------------------------------------------------

std::string_view trim(std::string_view text) {
  const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (not text.empty() and space(text.front())) text.remove_prefix(1);
  while (not text.empty() and space(text.back())) text.remove_suffix(1);
  return text;
}

// Everything from an unquoted `#` (at line start, or after whitespace) to the
// end of the line is a comment.
std::string_view strip_comment(std::string_view line) {
  char quote = '\0';
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quote != '\0') {
      if (c == quote) quote = '\0';
    } else if (c == '"' or c == '\'') {
      quote = c;
    } else if (c == '#' and (i == 0 or std::isspace(static_cast<unsigned char>(
                                           line[i - 1])) != 0)) {
      return line.substr(0, i);
    }
  }
  return line;
}

// One surrounding pair of matching quotes is removed; anything else is left as
// written.
std::string_view unquote(std::string_view value) {
  if (value.size() >= 2 and (value.front() == '"' or value.front() == '\'') and
      value.back() == value.front()) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

bool parse_bool(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lowered == "1" or lowered == "true" or lowered == "yes" or
         lowered == "on";
}

std::optional<int> parse_int(std::string_view value) {
  const std::string text(value);
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() or *end != '\0') return std::nullopt;
  return static_cast<int>(parsed);
}

// Assigns one KEY=VALUE pair. Returns false when KEY isn't one this app knows,
// so the caller can point out a likely typo.
bool apply_shellrc_pair(StartupSettings& settings, std::string_view key,
                        std::string_view value) {
  if (key == "MODEL")
    settings.model = std::string(value);
  else if (key == "POLICY")
    settings.policy = std::string(value);
  else if (key == "SKILLS_DIR")
    settings.skills_dir = std::string(value);
  else if (key == "SHELL")
    settings.shell = std::string(value);
  else if (key == "MODE_SWITCH_KEY")
    settings.mode_switch_key = std::string(value);
  else if (key == "MAX_STEPS")
    settings.max_steps = parse_int(value);
  else if (key == "NUM_CTX")
    settings.num_ctx = parse_int(value);
  else if (key == "SUMMARIZE_AT")
    settings.summarize_at = parse_int(value);
  else if (key == "ENABLE_SKILLS")
    settings.enable_skills = parse_bool(value);
  else if (key == "ENABLE_SUBAGENTS")
    settings.enable_subagents = parse_bool(value);
  else if (key == "ENABLE_PACKAGE_INSTALL")
    settings.enable_package_install = parse_bool(value);
  else if (key == "ENABLE_WEB_SEARCH")
    settings.enable_web_search = parse_bool(value);
  else
    return false;
  return true;
}

}  // namespace

StartupSettings load_startup_settings(const std::string& path,
                                      std::string& warning) {
  StartupSettings settings;

  const std::optional<std::string> text = read_file(path);
  if (not text) return settings;  // No file yet — not a warning.

  simdjson::ondemand::parser parser;
  simdjson::padded_string padded(*text);

  simdjson::ondemand::document document;
  if (parser.iterate(padded).get(document)) {
    warning = path + ": not valid JSON, ignoring";
    return settings;
  }

  simdjson::ondemand::object obj;
  if (document.get_object().get(obj)) {
    warning = path + ": top level is not a JSON object, ignoring";
    return settings;
  }

  settings.model = optional_string_field(obj, "model");
  settings.policy = optional_string_field(obj, "policy");
  settings.max_steps = optional_int_field(obj, "max_steps");
  settings.max_depth = optional_int_field(obj, "max_depth");
  settings.max_agents = optional_int_field(obj, "max_agents");
  settings.num_ctx = optional_int_field(obj, "num_ctx");
  settings.summarize_at = optional_int_field(obj, "summarize_at");
  settings.skills_dir = optional_string_field(obj, "skills_dir");
  settings.enable_skills = optional_bool_field(obj, "enable_skills");
  settings.enable_subagents = optional_bool_field(obj, "enable_subagents");
  settings.enable_package_install =
      optional_bool_field(obj, "enable_package_install");
  settings.enable_web_search = optional_bool_field(obj, "enable_web_search");
  settings.shell = optional_string_field(obj, "shell");
  settings.mode_switch_key = optional_string_field(obj, "mode_switch_key");

  return settings;
}

StartupSettings load_shellrc_settings(const std::string& path,
                                      std::string& warning) {
  StartupSettings settings;

  const std::optional<std::string> text = read_file(path);
  if (not text) return settings;  // No file yet — not a warning.

  std::vector<std::string> unknown;
  std::istringstream stream(*text);
  std::string raw;
  while (std::getline(stream, raw)) {
    std::string_view line = trim(strip_comment(raw));
    if (line.empty()) continue;

    if (line.rfind("export ", 0) == 0) line = trim(line.substr(7));

    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;  // Not an assignment.

    const std::string_view key = trim(line.substr(0, eq));
    const std::string_view value = unquote(trim(line.substr(eq + 1)));
    if (key.empty()) continue;
    if (not apply_shellrc_pair(settings, key, value)) {
      unknown.emplace_back(key);
    }
  }

  if (not unknown.empty()) {
    warning = path + ": ignored unknown key(s):";
    for (const std::string& key : unknown) warning += " " + key;
  }

  return settings;
}

}  // namespace agent
