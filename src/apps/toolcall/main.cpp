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

std::vector<std::string> tool_schemas() {
  return {
      agent::PythonTool().description(),
      agent::PackageInstallTool().description(),
      agent::WebSearchTool().description(),
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

agent::ToolResult dispatch(const std::string& name,
                           const agent::ToolArgs& args) {
  if (name == "python") return agent::PythonTool().execute(args);
  if (name == "package_install") return agent::PackageInstallTool().execute(args);
  if (name == "websearch") return agent::WebSearchTool().execute(args);

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

  // python / package_install target the workspace .m8trixenv; build and
  // activate it the same way the main agent does. websearch needs none of
  // this, but the bootstrap is cheap once the venv exists.
  const agent::VenvBootstrap venv = agent::create_workspace_venv();
  if (venv.status == agent::VenvBootstrap::Status::Failed) {
    std::cerr << "error: could not create the .m8trixenv virtualenv at "
              << venv.venv_dir << ": " << venv.detail << "\n";
    return 1;
  }
  if (venv.status == agent::VenvBootstrap::Status::NotAProject) {
    std::cerr << "note: not in a project directory; running against the base "
                 "Python\n";
  }

  const agent::ToolResult result = dispatch(name, args);
  print_result(result);
  return result.ok ? 0 : 1;
}
