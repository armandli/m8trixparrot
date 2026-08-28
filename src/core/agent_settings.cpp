#include <core/agent_settings.h>

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

  return settings;
}

}  // namespace agent
