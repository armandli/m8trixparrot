#include <core/mcp/resource_tool.h>

#include <optional>

#include <core/tools/tools_util.h>

namespace mcp {

std::string ResourceTool::description() {
  return R"json({"name":"mcp_resource","description":"Read context that MCP servers publish as resources (files, records, documents). action \"list\" shows what the servers offer, \"templates\" shows URI templates you can fill in yourself, and \"read\" fetches one resource by server and uri.","parameters":{"type":"object","properties":{"action":{"type":"string","enum":["list","templates","read"],"description":"What to do"},"server":{"type":"string","description":"The MCP server's name. Required for read when more than one server has resources; optional otherwise."},"uri":{"type":"string","description":"The resource URI to read"}},"required":["action"]}})json";
}

tools::ToolResult ResourceTool::execute(const tools::ToolArgs& args) const {
  const std::optional<std::string> action = tools::string_arg(args, "action");
  if (not action) {
    tools::ToolResult result;
    result.error = "mcp_resource needs an action: list, templates or read";
    return result;
  }
  return toolbox.resource(*action, tools::string_arg(args, "server").value_or(""),
                          tools::string_arg(args, "uri").value_or(""), context);
}

}  // namespace mcp
