#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include <core/tools.h>

int main(int argc, char** argv) {
  CLI::App app{
      "Fetch a URL and print its content to stdout.\n"
      "HTML is converted to readable markdown. Text and JSON are passed through.\n"
      "Download is capped at 10MB; text output truncated to 5000 lines / 100KB.\n"
      "The first output line reports the final URL and content type.\n"
      "\n"
      "Examples:\n"
      "  tool_webfetch https://example.com\n"
      "  tool_webfetch https://api.example.com/data.json"};

  std::string url;
  app.add_option("url", url, "URL to fetch")->required();

  CLI11_PARSE(app, argc, argv);

  agent::ToolArgs args;
  args.emplace("url", agent::ToolArgValue{url});

  const agent::ToolResult result = agent::WebFetchTool().execute(args);

  if (!result.ok) {
    std::cerr << result.error << "\n";
    return 1;
  }

  std::cout << result.output;
  if (!result.output.empty() && result.output.back() != '\n') std::cout << "\n";
  return 0;
}
