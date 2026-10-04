#include <core/mcp/json_rpc.h>

namespace mcp {

bool is_modern_error(int64_t code) {
  return code == kHeaderMismatch or code == kMissingRequiredClientCapability or
         code == kUnsupportedProtocolVersion;
}

RpcMessage parse_message(std::string_view text) {
  RpcMessage message;

  std::string error;
  std::optional<util::JsonValue> parsed = util::JsonValue::parse(text, &error);
  if (not parsed) {
    message.problem = "not JSON: " + error;
    return message;
  }
  if (not parsed->is_object()) {
    // A batch (array) is not part of MCP since 2025-06-18; nothing sends one.
    message.problem = "not a JSON-RPC object";
    return message;
  }

  const util::JsonValue& object = *parsed;
  if (const util::JsonValue* id = object.find("id")) message.id = *id;
  message.method = object.get("method").as_string();

  if (object.contains("method")) {
    message.params = object.get("params");
    message.kind = message.id.is_null() ? MessageKind::Notification
                                        : MessageKind::Request;
    return message;
  }
  if (const util::JsonValue* result = object.find("result")) {
    message.result = *result;
    message.kind = MessageKind::Result;
    return message;
  }
  if (const util::JsonValue* error_value = object.find("error")) {
    message.error.code = error_value->get("code").as_int(kInternalError);
    message.error.message = error_value->get("message").as_string();
    message.error.data = error_value->get("data");
    message.kind = MessageKind::Error;
    return message;
  }
  message.problem = "neither a request, a notification nor a response";
  return message;
}

namespace {

util::JsonValue envelope() {
  util::JsonValue out = util::JsonValue::object();
  out.set("jsonrpc", "2.0");
  return out;
}

}  // namespace

std::string make_request(const util::JsonValue& id, std::string_view method,
                         const util::JsonValue& params) {
  util::JsonValue out = envelope();
  out.set("id", id);
  out.set("method", method);
  if (not params.is_null()) out.set("params", params);
  return out.dump();
}

std::string make_notification(std::string_view method,
                              const util::JsonValue& params) {
  util::JsonValue out = envelope();
  out.set("method", method);
  if (not params.is_null()) out.set("params", params);
  return out.dump();
}

std::string make_result(const util::JsonValue& id,
                        const util::JsonValue& result) {
  util::JsonValue out = envelope();
  out.set("id", id);
  out.set("result", result.is_null() ? util::JsonValue::object() : result);
  return out.dump();
}

std::string make_error(const util::JsonValue& id, int64_t code,
                       std::string_view message) {
  util::JsonValue error = util::JsonValue::object();
  error.set("code", code);
  error.set("message", message);
  util::JsonValue out = envelope();
  out.set("id", id);
  out.set("error", std::move(error));
  return out.dump();
}

std::string id_key(const util::JsonValue& id) {
  if (id.is_string()) return "s:" + id.as_string();
  if (id.is_number()) return "n:" + std::to_string(id.as_int());
  return "x:" + id.dump();
}

std::string describe(const RpcError& error) {
  std::string out = std::to_string(error.code);
  if (not error.message.empty()) out += ": " + error.message;
  return out;
}

}  // namespace mcp
