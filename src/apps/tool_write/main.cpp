#include <iostream>
#include <sstream>
#include <string>

#include <CLI/CLI.hpp>

#include <core/tools.h>

int main(int argc, char** argv) {
  CLI::App app{
      "Write stdin to a file, creating parent directories as needed.\n"
      "Overwrites the file if it already exists.\n"
      "\n"
      "Examples:\n"
      "  tool_write src/main.cpp < new_main.cpp\n"
      "  tool_write config.json << 'EOF'\n"
      "  {\"key\": \"value\"}\n"
      "  EOF"};

  std::string path;
  app.add_option("path", path, "Destination file path")->required();

  CLI11_PARSE(app, argc, argv);

  // Read all of stdin as the file content.
  std::ostringstream buf;
  buf << std::cin.rdbuf();
  const std::string content = buf.str();

  agent::ToolArgs args;
  args.emplace("path",    agent::ToolArgValue{path});
  args.emplace("content", agent::ToolArgValue{content});

  const agent::ToolResult result = agent::WriteTool().execute(args);

  if (!result.ok) {
    std::cerr << result.error << "\n";
    return 1;
  }

  std::cout << result.output << "\n";
  return 0;
}
