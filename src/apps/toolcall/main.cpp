#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

#include <simdjson.h>

#include <core/json_util.h>
#include <core/tools.h>
#include <core/tools_util.h>

namespace {

// The tools this app exposes: exactly the ones in config/basic_tools.json.
// `memory`, `webfetch` and `websearch` are implemented in agentcore too, but
// they belong to the other config/*.json sets and are deliberately not enabled
// here.
//
// Each description() already returns that tool's entry from basic_tools.json
// verbatim, so nothing has to be kept in sync by hand.
std::vector<std::string> tool_schemas() {
  return {
      agent::BashTool().description(), agent::ReadTool().description(),
      agent::WriteTool().description(), agent::EditTool().description(),
      agent::FindTool().description(), agent::GrepTool().description(),
  };
}

// The schema strings are already valid JSON objects, so they are joined
// verbatim rather than pushed through JsonWriter, which would escape them into
// strings.
void print_schemas() {
  const std::vector<std::string> schemas = tool_schemas();

  std::string out = "[";
  for (size_t i = 0; i < schemas.size(); ++i) {
    if (i > 0) out += ",";
    out += schemas[i];
  }
  out += "]";

  std::cout << out << "\n";
}

// Same name -> tool mapping as BasicAgent::dispatch(), minus the tools this app
// doesn't enable. An unknown name is a failed ToolResult rather than a thrown
// error: it is still an answer about the call that was requested.
agent::ToolResult dispatch(const std::string& name,
                           const agent::ToolArgs& args) {
  if (name == "bash") return agent::BashTool().execute(args);
  if (name == "read") return agent::ReadTool().execute(args);
  if (name == "write") return agent::WriteTool().execute(args);
  if (name == "edit") return agent::EditTool().execute(args);
  if (name == "find") return agent::FindTool().execute(args);
  if (name == "grep") return agent::GrepTool().execute(args);

  agent::ToolResult unknown;
  unknown.error = "no tool named '" + name +
                  "' is enabled; run with --help to see the available tools";
  return unknown;
}

// Splits the call object into the tool name and the raw text of its arguments.
// The arguments stay as JSON text so they can go through args_from_json(),
// which is the one place in the codebase where JSON becomes ToolArgs.
//
// Returns false and sets `error` when the input isn't a JSON object carrying a
// non-empty "name".
bool parse_call(const std::string& json, std::string& name,
                std::string& arguments, std::string& error) {
  simdjson::ondemand::parser parser;
  simdjson::padded_string padded(json);

  simdjson::ondemand::document document;
  if (parser.iterate(padded).get(document)) {
    error = "argument is not valid JSON";
    return false;
  }

  simdjson::ondemand::object object;
  if (document.get_object().get(object)) {
    error = "argument is not a JSON object";
    return false;
  }

  name = agent::string_field(object, "name");
  if (name.empty()) {
    error = "call object has no \"name\" string naming the tool to run";
    return false;
  }

  // An absent "arguments" means the call takes none — valid for `ls`, whose
  // parameters are all optional.
  arguments = agent::raw_field(object, "arguments", "{}");
  return true;
}

// The whole ToolResult, so a caller sees the truncation flags rather than
// having to guess whether output was clipped. `output` and `error` are always
// present so they can be indexed unconditionally; `overflow_path` only appears
// when there is a file to point at.
void print_result(const agent::ToolResult& result) {
  agent::JsonWriter writer;
  writer.begin_object()
      .field("ok", result.ok)
      .field("output", result.output)
      .field("error", result.error)
      .field("truncated", result.truncated);
  if (not result.overflow_path.empty()) {
    writer.field("overflow_path", result.overflow_path);
  }
  writer.end_object();

  std::cout << writer.str() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{
      "m8trixparrot toolcall - run one tool call given as JSON and print the "
      "result as JSON"};

  // --help prints the enabled tools' schemas as a JSON array and nothing else,
  // so its output can be fed straight into a model's `tools` parameter. That
  // means replacing CLI11's built-in help flag rather than adding alongside it.
  bool show_schemas = false;
  app.set_help_flag();
  app.add_flag("-h,--help", show_schemas,
               "Print the enabled tool schemas as a JSON array");

  std::string call_json;
  app.add_option("call", call_json,
                 "Tool call as JSON: {\"name\":\"...\",\"arguments\":{...}}");

  CLI11_PARSE(app, argc, argv);

  if (show_schemas) {
    print_schemas();
    return 0;
  }

  // Bad input is reported on stderr and never on stdout: stdout carries either
  // a schema array or a ToolResult, never a third shape.
  if (call_json.empty()) {
    std::cerr << "error: no tool call given\n"
              << app.help() << "\n";
    return 2;
  }

  std::string name;
  std::string arguments;
  std::string error;
  if (not parse_call(call_json, name, arguments, error)) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }

  const agent::ToolArgs args = agent::args_from_json(arguments, error);
  if (not error.empty()) {
    std::cerr << "error: " << error << "\n";
    return 2;
  }

  const agent::ToolResult result = dispatch(name, args);
  print_result(result);
  return result.ok ? 0 : 1;
}
