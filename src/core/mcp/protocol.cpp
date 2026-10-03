#include <core/mcp/protocol.h>

namespace mcp {

namespace {

std::optional<bool> optional_bool(const util::JsonValue& value) {
  if (not value.is_bool()) return std::nullopt;
  return value.as_bool();
}

// A capability is present when its key is (any object, even empty).
bool has(const util::JsonValue& capabilities, std::string_view key) {
  return capabilities.find(key) != nullptr;
}

}  // namespace

bool is_known_legacy_version(std::string_view version) {
  return version == "2025-11-25" or version == "2025-06-18" or
         version == "2025-03-26" or version == "2024-11-05";
}

const char* era_name(Era era) {
  switch (era) {
    case Era::Modern:
      return "modern";
    case Era::Legacy:
      return "legacy";
    case Era::Unknown:
    default:
      return "unknown";
  }
}

ServerCapabilities parse_capabilities(const util::JsonValue& capabilities) {
  ServerCapabilities out;
  out.tools = has(capabilities, "tools");
  out.tools_list_changed =
      capabilities.get("tools").get("listChanged").as_bool(false);
  out.resources = has(capabilities, "resources");
  out.resources_subscribe =
      capabilities.get("resources").get("subscribe").as_bool(false);
  out.resources_list_changed =
      capabilities.get("resources").get("listChanged").as_bool(false);
  out.prompts = has(capabilities, "prompts");
  out.prompts_list_changed =
      capabilities.get("prompts").get("listChanged").as_bool(false);
  return out;
}

std::vector<ToolInfo> parse_tools(const util::JsonValue& tools) {
  std::vector<ToolInfo> out;
  for (const util::JsonValue& tool : tools.items()) {
    ToolInfo info;
    info.name = tool.get("name").as_string();
    if (info.name.empty()) continue;
    info.title = tool.get("title").as_string();
    // Pre-2025-06-18 servers put the display name in annotations.title.
    if (info.title.empty()) {
      info.title = tool.get("annotations").get("title").as_string();
    }
    info.description = tool.get("description").as_string();
    info.input_schema = tool.get("inputSchema");
    info.has_output_schema = tool.contains("outputSchema");
    const util::JsonValue& annotations = tool.get("annotations");
    info.annotations.read_only = optional_bool(annotations.get("readOnlyHint"));
    info.annotations.destructive =
        optional_bool(annotations.get("destructiveHint"));
    info.annotations.idempotent =
        optional_bool(annotations.get("idempotentHint"));
    info.annotations.open_world =
        optional_bool(annotations.get("openWorldHint"));
    out.push_back(std::move(info));
  }
  return out;
}

std::vector<ResourceInfo> parse_resources(const util::JsonValue& resources) {
  std::vector<ResourceInfo> out;
  for (const util::JsonValue& resource : resources.items()) {
    ResourceInfo info;
    info.uri = resource.get("uri").as_string();
    if (info.uri.empty()) continue;
    info.name = resource.get("name").as_string();
    info.title = resource.get("title").as_string();
    info.description = resource.get("description").as_string();
    info.mime_type = resource.get("mimeType").as_string();
    info.size = resource.get("size").as_int(-1);
    out.push_back(std::move(info));
  }
  return out;
}

std::vector<ResourceTemplateInfo> parse_resource_templates(
    const util::JsonValue& templates) {
  std::vector<ResourceTemplateInfo> out;
  for (const util::JsonValue& entry : templates.items()) {
    ResourceTemplateInfo info;
    info.uri_template = entry.get("uriTemplate").as_string();
    if (info.uri_template.empty()) continue;
    info.name = entry.get("name").as_string();
    info.title = entry.get("title").as_string();
    info.description = entry.get("description").as_string();
    info.mime_type = entry.get("mimeType").as_string();
    out.push_back(std::move(info));
  }
  return out;
}

std::vector<PromptInfo> parse_prompts(const util::JsonValue& prompts) {
  std::vector<PromptInfo> out;
  for (const util::JsonValue& prompt : prompts.items()) {
    PromptInfo info;
    info.name = prompt.get("name").as_string();
    if (info.name.empty()) continue;
    info.title = prompt.get("title").as_string();
    info.description = prompt.get("description").as_string();
    for (const util::JsonValue& argument : prompt.get("arguments").items()) {
      PromptArgument arg;
      arg.name = argument.get("name").as_string();
      if (arg.name.empty()) continue;
      arg.description = argument.get("description").as_string();
      arg.required = argument.get("required").as_bool(false);
      info.arguments.push_back(std::move(arg));
    }
    out.push_back(std::move(info));
  }
  return out;
}

}  // namespace mcp
