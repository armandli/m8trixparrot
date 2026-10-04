#ifndef M8_MCP_PROTOCOL_H
#define M8_MCP_PROTOCOL_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/util/json_value.h>

// The Model Context Protocol, client side.
//
// m8 speaks MCP 2026-07-28, which is stateless — every request carries its
// protocol version, client identity and capabilities in `_meta`, and there is
// no `initialize` handshake — and falls back to the handshake-based "legacy"
// revisions (2024-11-05 through 2025-11-25) that most servers still run. The
// spec calls a client that does both "dual-era"; see client.h for how the era
// of a server is detected.
//
// This header holds the plain data the rest of the component passes around.
namespace mcp {

inline constexpr const char* kModernVersion = "2026-07-28";
// What a legacy `initialize` asks for; a server may answer with any older
// version it prefers.
inline constexpr const char* kLatestLegacyVersion = "2025-11-25";

inline constexpr const char* kClientName = "m8";
inline constexpr const char* kClientVersion = "0.1.0";

// True for a revision whose shape this client implements on the legacy path.
bool is_known_legacy_version(std::string_view version);

enum struct Era : uint8_t { Unknown, Modern, Legacy };

const char* era_name(Era era);

// Behavioural hints a server attaches to a tool. Untrusted unless the server
// is: they are shown, never acted on.
struct ToolAnnotations {
  std::optional<bool> read_only;
  std::optional<bool> destructive;
  std::optional<bool> idempotent;
  std::optional<bool> open_world;
};

struct ToolInfo {
  std::string name;  // exactly as the server spells it; tools/call uses this
  std::string title;
  std::string description;
  util::JsonValue input_schema;  // as received
  bool has_output_schema = false;
  ToolAnnotations annotations;
};

struct ResourceInfo {
  std::string uri;
  std::string name;
  std::string title;
  std::string description;
  std::string mime_type;
  int64_t size = -1;  // bytes; -1 when the server did not say
};

struct ResourceTemplateInfo {
  std::string uri_template;
  std::string name;
  std::string title;
  std::string description;
  std::string mime_type;
};

struct PromptArgument {
  std::string name;
  std::string description;
  bool required = false;
};

struct PromptInfo {
  std::string name;
  std::string title;
  std::string description;
  std::vector<PromptArgument> arguments;
};

// What a server said it supports, from `server/discover` (modern) or the
// `initialize` result (legacy).
struct ServerCapabilities {
  bool tools = false;
  bool tools_list_changed = false;
  bool resources = false;
  bool resources_subscribe = false;
  bool resources_list_changed = false;
  bool prompts = false;
  bool prompts_list_changed = false;
};

ServerCapabilities parse_capabilities(const util::JsonValue& capabilities);

// Self-reported and unverified; for display only.
struct ServerInfo {
  std::string name;
  std::string version;
  std::string title;
};

// Which agent a call is made on behalf of, so a question a server raises can be
// labelled with who caused it.
struct CallContext {
  std::string agent_id;
  std::string agent_label;  // "root" | "subagent ..."; empty for the user
};

// A server asking the user for input (MCP elicitation), in either mode.
struct ElicitationRequest {
  std::string server;
  std::string scope;  // where its config came from: "user" | "shared" | "project"
  std::string agent_label;
  std::string mode = "form";  // "form" | "url"
  std::string message;
  util::JsonValue requested_schema;  // form mode
  std::string url;                   // url mode
};

struct ElicitationResult {
  std::string action = "cancel";  // "accept" | "decline" | "cancel"
  util::JsonValue content;        // form mode, on accept
};

// Runs on whichever thread is waiting for the server's answer, and may block
// on a human for as long as it takes. Unset means the client declares no
// elicitation capability, so a compliant server never asks.
using ElicitationHandler =
    std::function<ElicitationResult(const ElicitationRequest&)>;

// Parsers for the list results. Each skips entries missing what makes them
// usable (a tool with no name, a resource with no uri).
std::vector<ToolInfo> parse_tools(const util::JsonValue& tools);
std::vector<ResourceInfo> parse_resources(const util::JsonValue& resources);
std::vector<ResourceTemplateInfo> parse_resource_templates(
    const util::JsonValue& templates);
std::vector<PromptInfo> parse_prompts(const util::JsonValue& prompts);

}  // namespace mcp

#endif  // M8_MCP_PROTOCOL_H
