#ifndef M8_MCP_RESOURCE_TOOL_H
#define M8_MCP_RESOURCE_TOOL_H

#include <string>

#include <core/mcp/protocol.h>
#include <core/mcp/toolbox.h>
#include <core/tools/tools.h>

namespace mcp {

// `mcp_resource`: the context MCP servers publish as resources (files,
// records, documents), one tool with three actions in the style of `skill`
// and `bash_search`. Offered only while a connected server declares the
// resources capability, and never deferred: it is small, and a model cannot
// search for a tool it does not know to want.
struct ResourceTool {
  Toolbox& toolbox;
  const CallContext& context;

  static std::string description();
  // action (list | templates | read, required), server, uri.
  tools::ToolResult execute(const tools::ToolArgs& args) const;
};

}  // namespace mcp

#endif  // M8_MCP_RESOURCE_TOOL_H
