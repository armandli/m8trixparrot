#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include <core/tools/tools.h>

int main(int argc, char** argv) {
  CLI::App app{
      "Find files by glob pattern, respecting .gitignore.\n"
      "Output is one relative path per line, sorted, capped at 1000 results.\n"
      "\n"
      "Examples:\n"
      "  tool_find '**/*.cpp'\n"
      "  tool_find '**/*.h' --path src/ --limit 50\n"
      "  tool_find 'test_*' --path test/"};

  std::string pattern;
  app.add_option("pattern", pattern,
                 "Glob pattern (e.g. '**/*.cpp', 'src/**/*.h')")->required();

  std::string path;
  app.add_option("--path,-p", path,
                 "Directory to search (default: current working directory)");

  int64_t limit = 1000;
  app.add_option("--limit,-n", limit, "Maximum number of results (default: 1000)");

  CLI11_PARSE(app, argc, argv);

  tools::ToolArgs args;
  args.emplace("pattern", tools::ToolArgValue{pattern});
  if (!path.empty()) args.emplace("path", tools::ToolArgValue{path});
  args.emplace("limit", tools::ToolArgValue{limit});

  const tools::ToolResult result = tools::FindTool().execute(args);

  if (!result.ok) {
    std::cerr << result.error << "\n";
    return 1;
  }

  std::cout << result.output;
  return 0;
}
