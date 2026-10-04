#ifndef M8_MCP_VIEW_H
#define M8_MCP_VIEW_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <core/mcp/catalog.h>
#include <core/mcp/config.h>
#include <core/mcp/tool_search.h>
#include <core/util/json_value.h>

// What m8 shows about MCP — the /mcp report, /mcp tools, the /context line,
// the header tag, prompt commands — as pure functions over a catalog snapshot,
// so all of it is testable without a terminal or a server.
namespace m8 {

// "1.6k" / "640".
std::string human_tokens(int64_t tokens);

// A header or env value for display: `${VAR}` references are shown as
// written, and literal text as dots unless it is only an auth scheme
// ("Bearer ") or punctuation — `m8 mcp get` never prints a token.
std::string mask_secret(const std::string& value);

// "mcp 3/4", with " !" when a server waits for approval or a login.
// Empty when no server is configured.
std::string mcp_header(const mcp::Catalog& catalog);

// The /mcp report: one line per server, then the tool-search state.
std::string mcp_status_text(const mcp::Catalog& catalog,
                            const mcp::ToolSearchSettings& settings,
                            int64_t context_window, bool deferred,
                            const std::vector<std::string>& loaded);

// /mcp tools [server]: every tool, marked loaded, deferred or always-loaded —
// or, without `status`, just listed (`m8 mcp get`, where no agent is running).
std::string mcp_tools_text(const mcp::Catalog& catalog, const std::string& server,
                           bool deferred, const std::vector<std::string>& loaded,
                           bool status = true);

// The /context line: "mcp: 57 tools · loaded 5 (~1.6k) · deferred 52 (~7.9k)".
std::string mcp_context_line(const mcp::Catalog& catalog, bool deferred,
                             const std::vector<std::string>& loaded);

// Servers waiting for approval, as one notice (empty when none).
std::string pending_approval_text(const mcp::Catalog& catalog);

// The /help lines for MCP prompts, which run as /mcp__<server>__<prompt>.
std::string prompt_help_text(const mcp::Catalog& catalog);

// "/mcp__fake__review <code> [lang]".
std::string prompt_usage(const std::string& server, const mcp::PromptInfo& prompt);

// A parsed /mcp__<server>__<prompt> line. nullopt when the line names no
// known prompt; otherwise `error` is set when its arguments do not fit.
struct PromptCommand {
  std::string server;
  std::string prompt;
  util::JsonValue arguments;  // {name: string}
  std::string error;
};

std::optional<PromptCommand> parse_prompt_command(const std::string& line,
                                                  const mcp::Catalog& catalog);

// The slash-command name for a prompt: mcp__<server>__<prompt>, sanitized.
std::string prompt_command_name(const std::string& server, const std::string& prompt);

// `m8 mcp get <name>`: the entry, with secrets masked.
std::string server_config_text(const mcp::ServerConfig& config);

// A tool call's header in the transcript: mcp__github__create_issue reads as
// "github › create_issue"; any other name is shown as is.
std::string tool_display_name(const std::string& name);

}  // namespace m8

#endif  // M8_MCP_VIEW_H
