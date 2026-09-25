#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include <core/tools/tools.h>

int main(int argc, char** argv) {
  CLI::App app{
      "Read a file and print its contents to stdout.\n"
      "Text output is truncated to 5000 lines / 100KB; use --offset to continue\n"
      "past the cut. Images (jpg/png/gif/webp/bmp) are emitted as base64."};

  std::string path;
  app.add_option("path", path, "File to read")->required();

  int64_t offset = 1;
  app.add_option("--offset,-o", offset,
                 "First line to include, 1-indexed (default: 1)");

  int64_t limit = 0;
  app.add_option("--limit,-n", limit,
                 "Maximum number of lines to return (default: 0 = all)");

  CLI11_PARSE(app, argc, argv);

  tools::ToolArgs args;
  args.emplace("path",   tools::ToolArgValue{path});
  args.emplace("offset", tools::ToolArgValue{offset});
  args.emplace("limit",  tools::ToolArgValue{limit});

  const tools::ToolResult result = tools::ReadTool().execute(args);

  if (!result.ok) {
    std::cerr << result.error << "\n";
    return 1;
  }

  std::cout << result.output;
  // Ensure the output ends with a newline if it doesn't already so the shell
  // prompt lands on a fresh line (file content sometimes lacks a trailing \n).
  if (!result.output.empty() && result.output.back() != '\n') {
    std::cout << "\n";
  }
  return 0;
}
