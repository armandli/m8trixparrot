#ifndef M8_MCP_TOOLBOX_H
#define M8_MCP_TOOLBOX_H

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <core/mcp/catalog.h>
#include <core/mcp/protocol.h>
#include <core/tools/tools.h>

namespace mcp {

// What an agent needs from MCP, and nothing more. The registry implements it
// for real; agent tests implement it with no processes at all. An agent holds
// it through AgentOptions::mcp, so every subagent shares its parent's
// connections instead of starting servers of its own.
struct Toolbox {
  virtual ~Toolbox() = default;

  // The current servers and tools. Immutable; hold it as long as needed.
  virtual std::shared_ptr<const Catalog> snapshot() const = 0;

  // Waits (up to `limit`) for servers still connecting, then returns the
  // newest snapshot. tool_search calls this so a model searching right after
  // startup does not miss a server that is a second from ready.
  virtual std::shared_ptr<const Catalog> wait_until_settled(
      std::chrono::milliseconds limit) = 0;

  // Calls a tool by its exposed name with the model's raw argument JSON.
  // A tool that reports failure (isError) comes back ok == false with the
  // server's words in `error`, which is what the model should read.
  virtual tools::ToolResult call_tool(const std::string& exposed,
                                      std::string_view arguments_json,
                                      const CallContext& context) = 0;

  // The mcp_resource tool's back end: action "list", "templates" or "read".
  virtual tools::ToolResult resource(std::string_view action,
                                     const std::string& server,
                                     const std::string& uri,
                                     const CallContext& context) = 0;
};

}  // namespace mcp

#endif  // M8_MCP_TOOLBOX_H
