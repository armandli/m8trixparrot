#ifndef M8_MCP_JSON_RPC_H
#define M8_MCP_JSON_RPC_H

#include <cstdint>
#include <string>
#include <string_view>

#include <core/util/json_value.h>

namespace mcp {

// JSON-RPC 2.0 codes MCP uses. The -32020..-32022 range is reserved for the
// MCP specification (2026-07-28) and is what tells a modern server from a
// legacy one when a probe fails.
inline constexpr int64_t kParseError = -32700;
inline constexpr int64_t kInvalidRequest = -32600;
inline constexpr int64_t kMethodNotFound = -32601;
inline constexpr int64_t kInvalidParams = -32602;
inline constexpr int64_t kInternalError = -32603;
inline constexpr int64_t kHeaderMismatch = -32020;
inline constexpr int64_t kMissingRequiredClientCapability = -32021;
inline constexpr int64_t kUnsupportedProtocolVersion = -32022;
// Codes earlier revisions defined, still accepted from legacy servers.
inline constexpr int64_t kLegacyResourceNotFound = -32002;
inline constexpr int64_t kLegacyUrlElicitationRequired = -32042;

// An error code only a 2026-07-28-or-later server would send.
bool is_modern_error(int64_t code);

enum struct MessageKind : uint8_t {
  Request,       // method + id
  Notification,  // method, no id
  Result,        // id + result
  Error,         // id (maybe null) + error
  Invalid,       // anything else, including malformed JSON
};

struct RpcError {
  int64_t code = 0;
  std::string message;
  util::JsonValue data;
};

struct RpcMessage {
  MessageKind kind = MessageKind::Invalid;
  util::JsonValue id;  // number or string; null for notifications
  std::string method;
  util::JsonValue params;
  util::JsonValue result;
  RpcError error;
  std::string problem;  // why it is Invalid
};

// One message from the wire. Never throws; malformed input is Invalid.
RpcMessage parse_message(std::string_view text);

// Wire text for one message, compact and on one line (stdio frames by
// newline, so a message must never contain a raw one — JSON escaping
// guarantees it).
std::string make_request(const util::JsonValue& id, std::string_view method,
                         const util::JsonValue& params);
std::string make_notification(std::string_view method,
                              const util::JsonValue& params);
std::string make_result(const util::JsonValue& id,
                        const util::JsonValue& result);
std::string make_error(const util::JsonValue& id, int64_t code,
                       std::string_view message);

// A canonical key for matching a response to its request. Ids come back as
// the server echoes them, so 7 and "7" must stay distinct.
std::string id_key(const util::JsonValue& id);

// "code: message" for showing to a model or a person.
std::string describe(const RpcError& error);

}  // namespace mcp

#endif  // M8_MCP_JSON_RPC_H
