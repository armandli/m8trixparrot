#ifndef M8_MCP_SCHEMA_H
#define M8_MCP_SCHEMA_H

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/util/json_value.h>

namespace mcp {

// ---------------------------------------------------------------------------
// Lowering an MCP tool's inputSchema into something Ollama will take.
//
// MCP allows any JSON Schema 2020-12 in `inputSchema`. Ollama (checked against
// 0.35.1's api/types.go) unmarshals a tool's `parameters` into fixed Go structs:
// the root `type` must be a string; a property may carry only `type` (a string
// or an array of strings), `description`, `enum`, `items`, `properties`,
// `required` and `anyOf`. Every other keyword is dropped without a word — so a
// `$ref` silently becomes an untyped field — and a shape the structs cannot
// hold (a boolean sub-schema, a `type` array at the root, a non-string in
// `required`) fails the WHOLE /api/chat request, taking every tool with it.
//
// So each schema is rewritten before Ollama sees it: local `$ref`s are inlined,
// `oneOf` becomes `anyOf`, `allOf` is merged, boolean schemas are resolved,
// and what Ollama would drop (`default`, `format`, ranges, `pattern`, ...) is
// folded into the description where the model can still read it. The server
// still receives the model's arguments untouched and validates them against
// its real schema.
// ---------------------------------------------------------------------------

struct LoweredSchema {
  util::JsonValue parameters;      // always {"type":"object", ...}
  std::vector<std::string> notes;  // what was approximated, for the log
};

LoweredSchema lower_schema(const util::JsonValue& input_schema);

// Empty when `parameters` fits Ollama's ToolFunctionParameters / ToolProperty
// shapes; otherwise the first reason it would not. A lowered schema failing
// this is a lowering bug, and the tool is withheld rather than sent.
std::string ollama_shape_problem(const util::JsonValue& parameters);

// The {"name","description","parameters"} object Agent::tool_schemas() hands
// to the Ollama client. The description is repaired UTF-8 and capped.
std::string tool_schema_json(std::string_view name, std::string_view description,
                             const util::JsonValue& parameters);

// "name(path: string, recursive?: boolean, mode: \"r\"|\"w\")" — what
// tool_search shows the model for a tool it just loaded.
std::string tool_signature(std::string_view name,
                           const util::JsonValue& parameters,
                           size_t limit = 400);

// ---------------------------------------------------------------------------
// Arguments.
// ---------------------------------------------------------------------------

// The model's raw `arguments` as an object. Accepts what small models actually
// send: nothing at all, `null`, or the object JSON-encoded inside a string.
// Anything else is an error the model is told about.
util::JsonValue arguments_object(std::string_view raw, std::string& error);

// Coerces values toward the (lowered) schema: "5" to 5 for a number, "true" to
// true for a boolean, a JSON-encoded object or array back into one, a number to
// a string where only a string is allowed. Each change is described in `fixes`
// when it is non-null. The server still has the last word.
util::JsonValue fix_arguments(const util::JsonValue& arguments,
                              const util::JsonValue& parameters,
                              std::vector<std::string>* fixes = nullptr);

// ---------------------------------------------------------------------------
// x-mcp-header (Streamable HTTP, 2026-07-28): parameters a server wants
// mirrored into `Mcp-Param-<name>` request headers.
// ---------------------------------------------------------------------------

struct HeaderParam {
  std::vector<std::string> path;  // properties chain from the schema root
  std::string name;               // the <name> in Mcp-Param-<name>
};

// Collects every annotation in a tool's raw inputSchema into `out`. Returns an
// empty string when the schema is usable — including when it has none — and
// otherwise why the tool must be excluded (the spec requires dropping a tool
// with an invalid annotation rather than failing the whole list).
std::string collect_header_params(const util::JsonValue& input_schema,
                                  std::vector<HeaderParam>& out);

// (header name, encoded value) pairs for one call. A parameter absent or null
// in `arguments` produces no header.
std::vector<std::pair<std::string, std::string>> header_values(
    const std::vector<HeaderParam>& params, const util::JsonValue& arguments);

// A header value as the spec wants it: plain when it is visible ASCII without
// edge whitespace, otherwise =?base64?<base64 of the UTF-8>?=.
std::string encode_header_value(std::string_view value);

}  // namespace mcp

#endif  // M8_MCP_SCHEMA_H
