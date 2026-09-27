#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include <core/tools/tools.h>

namespace {

// The only place this command will look for a key. The help text below names the
// same path, so the two have to be changed together.
constexpr const char* kApiKeyFile = "~/.parallel_api_key";

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{
      "Search the web and return a numbered list of results.\n"
      "Each result includes a title, URL, and a short text snippet.\n"
      "Reads the Parallel API key from ~/.parallel_api_key and nowhere else;\n"
      "PARALLEL_API_KEY and .m8/parallel_api_key are ignored here.\n"
      "URLs in the output can be passed directly to tool_webfetch.\n"
      "Output is truncated at 100KB; exit 1 on error or missing API key.\n"
      "\n"
      "Examples:\n"
      "  tool_websearch 'C++20 coroutines tutorial'\n"
      "  tool_websearch 'openssl CVE 2024' --limit 5"};

  std::string query;
  app.add_option("query", query, "Search query")->required();

  int64_t limit = 10;
  app.add_option("--limit,-n", limit,
                 "Maximum number of results to return (default: 10)");

  CLI11_PARSE(app, argc, argv);

  // Before any search: this command is run from wherever the operator happens to
  // be standing, so a workspace-relative key file would usually be missing and
  // an inherited PARALLEL_API_KEY would be an invisible source.
  tools::set_web_search_api_key_file(kApiKeyFile);

  tools::ToolArgs args;
  args.emplace("query", tools::ToolArgValue{query});
  args.emplace("limit", tools::ToolArgValue{limit});

  const tools::ToolResult result = tools::WebSearchTool().execute(args);

  if (!result.ok) {
    std::cerr << result.error << "\n";
    return 1;
  }

  std::cout << result.output;
  if (!result.output.empty() && result.output.back() != '\n') std::cout << "\n";
  return 0;
}
