#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include <core/tools.h>

int main(int argc, char** argv) {
  CLI::App app{
      "Search file contents by pattern. Respects .gitignore.\n"
      "Match lines are printed as  file:line:content.\n"
      "Context lines use  file-line-content  (dash separator, not colon).\n"
      "Output is capped at 100 matches / 100KB; lines truncated at 2000 chars.\n"
      "\n"
      "Examples:\n"
      "  tool_grep 'TODO'\n"
      "  tool_grep 'execute' --path src/ --glob '**/*.cpp' -C 2\n"
      "  tool_grep 'BashTool' --literal --ignore-case --limit 20\n"
      "  tool_grep '^class ' src/core/agent.h"};

  std::string pattern;
  app.add_option("pattern", pattern,
                 "Regex (default) or literal search pattern")->required();

  std::string path;
  app.add_option("--path,-p", path,
                 "File or directory to search (default: current working directory)");

  std::string glob;
  app.add_option("--glob,-g", glob,
                 "Only search files matching this glob pattern (e.g. '**/*.cpp')");

  bool ignore_case = false;
  app.add_flag("--ignore-case,-i", ignore_case, "Case-insensitive matching");

  bool literal = false;
  app.add_flag("--literal,-l", literal,
               "Treat pattern as a literal string, not a regex");

  int64_t context = 0;
  app.add_option("--context,-C", context,
                 "Lines of context to print around each match (default: 0)");

  int64_t limit = 100;
  app.add_option("--limit,-n", limit, "Maximum number of matches (default: 100)");

  CLI11_PARSE(app, argc, argv);

  agent::ToolArgs args;
  args.emplace("pattern", agent::ToolArgValue{pattern});
  if (!path.empty())   args.emplace("path",       agent::ToolArgValue{path});
  if (!glob.empty())   args.emplace("glob",        agent::ToolArgValue{glob});
  if (ignore_case)     args.emplace("ignoreCase",  agent::ToolArgValue{ignore_case});
  if (literal)         args.emplace("literal",     agent::ToolArgValue{literal});
  args.emplace("context", agent::ToolArgValue{context});
  args.emplace("limit",   agent::ToolArgValue{limit});

  const agent::ToolResult result = agent::GrepTool().execute(args);

  if (!result.ok) {
    std::cerr << result.error << "\n";
    return 1;
  }

  // Exit 1 when there are no matches (same convention as /usr/bin/grep) so
  // agents can use the exit code as a reliable "found something" signal.
  const bool no_matches = (result.output == "[no matches]");
  std::cout << result.output;
  if (!result.output.empty() && result.output.back() != '\n') std::cout << "\n";
  return no_matches ? 1 : 0;
}
