#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <simdjson.h>

#include <core/tools/tools.h>

// Reads the edits JSON array from stdin and populates `pairs`.
// The expected format is:
//   [{"oldText": "...", "newText": "..."}, ...]
// Returns an error string, or empty on success.
static std::string parse_edits_json(
    const std::string& json,
    std::vector<std::pair<std::string, std::string>>& pairs) {
  simdjson::ondemand::parser parser;
  simdjson::padded_string padded(json);

  simdjson::ondemand::document doc;
  if (parser.iterate(padded).get(doc)) {
    return "stdin is not valid JSON";
  }

  simdjson::ondemand::array arr;
  if (doc.get_array().get(arr)) {
    return "expected a JSON array of {\"oldText\": ..., \"newText\": ...} objects";
  }

  size_t index = 0;
  for (auto element : arr) {
    simdjson::ondemand::object obj;
    if (element.get_object().get(obj)) {
      return "element " + std::to_string(index) + " is not a JSON object";
    }

    std::string old_text;
    std::string new_text;
    bool has_old = false;
    bool has_new = false;

    for (auto field : obj) {
      std::string_view key;
      if (field.unescaped_key().get(key)) continue;
      std::string_view text;
      if (field.value().get_string().get(text)) continue;

      if (key == "oldText") { old_text = text; has_old = true; }
      else if (key == "newText") { new_text = text; has_new = true; }
    }

    if (!has_old) {
      return "element " + std::to_string(index) + " is missing \"oldText\"";
    }
    if (!has_new) {
      return "element " + std::to_string(index) + " is missing \"newText\"";
    }

    pairs.emplace_back(std::move(old_text), std::move(new_text));
    ++index;
  }

  if (pairs.empty()) {
    return "edits array is empty";
  }
  return {};
}

int main(int argc, char** argv) {
  CLI::App app{
      "Apply exact-text replacements to a file.\n"
      "Reads a JSON array of {\"oldText\": ..., \"newText\": ...} pairs from stdin.\n"
      "Every oldText must appear exactly once in the file (non-overlapping).\n"
      "All edits are validated before any change is written.\n"
      "\n"
      "Examples:\n"
      "  printf '[{\"oldText\":\"return 0;\",\"newText\":\"return 1;\"}]' \\\n"
      "    | tool_edit src/main.cpp\n"
      "\n"
      "  tool_edit src/main.cpp << 'EOF'\n"
      "  [{\"oldText\":\"void foo()\",\"newText\":\"void foo(int x)\"},\n"
      "   {\"oldText\":\"int bar = 0;\",\"newText\":\"int bar = x;\"}]\n"
      "  EOF"};

  std::string path;
  app.add_option("path", path, "File to edit")->required();

  CLI11_PARSE(app, argc, argv);

  // Read the JSON edits array from stdin.
  std::ostringstream buf;
  buf << std::cin.rdbuf();
  const std::string json = buf.str();

  if (json.empty()) {
    std::cerr << "tool_edit: no input on stdin; "
                 "expected a JSON array of {oldText, newText} pairs\n";
    return 1;
  }

  std::vector<std::pair<std::string, std::string>> pairs;
  const std::string parse_error = parse_edits_json(json, pairs);
  if (!parse_error.empty()) {
    std::cerr << "tool_edit: " << parse_error << "\n";
    return 1;
  }

  tools::ToolArgs args;
  args.emplace("path",  tools::ToolArgValue{path});
  args.emplace("edits", tools::ToolArgValue{std::move(pairs)});

  const tools::ToolResult result = tools::EditTool().execute(args);

  if (!result.ok) {
    std::cerr << result.error << "\n";
    return 1;
  }

  std::cout << result.output << "\n";
  return 0;
}
