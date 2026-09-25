#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include <core/tools/tools.h>

int main(int argc, char** argv) {
  CLI::App app{
      "Search the web and return a numbered list of results.\n"
      "Each result includes a title, URL, and a short text snippet.\n"
      "Requires PARALLEL_API_KEY env var or .m8trix/parallel_api_key file.\n"
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
