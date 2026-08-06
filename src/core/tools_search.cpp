#include <core/tools.h>

#include <cctype>
#include <cstdint>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <core/tools_util.h>

namespace agent {

namespace {

// grep clips individual lines at this width so one minified file can't eat the
// whole budget.
constexpr size_t kMaxLineWidth = 2000;

std::string default_path(const ToolArgs& args) {
  const std::optional<std::string> path = string_arg(args, "path");
  if (path and not path->empty()) return *path;
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  return ec ? std::string(".") : cwd.string();
}

// Translates a glob into an equivalent ECMAScript regex:
//   **/  matches any number of leading directories (including none)
//   **   matches anything, path separators included
//   *    matches anything within one path segment
//   ?    matches one character within one path segment
std::string glob_to_regex(std::string_view glob) {
  std::string regex = "^";

  for (size_t i = 0; i < glob.size(); ++i) {
    const char c = glob[i];
    if (c == '*') {
      const bool is_double = (i + 1 < glob.size() and glob[i + 1] == '*');
      if (is_double) {
        ++i;
        if (i + 1 < glob.size() and glob[i + 1] == '/') {
          ++i;
          regex += "(?:.*/)?";
        } else {
          regex += ".*";
        }
      } else {
        regex += "[^/]*";
      }
    } else if (c == '?') {
      regex += "[^/]";
    } else if (std::string_view("\\^$.|+()[]{}").find(c) !=
               std::string_view::npos) {
      regex += '\\';
      regex += c;
    } else {
      regex += c;
    }
  }

  regex += "$";
  return regex;
}

// Every non-ignored regular file under `root`, as paths relative to it, in
// directory-iteration order. Ignored directories aren't descended into at all,
// so a large build/ costs nothing to skip.
std::vector<std::string> walk_files(const std::filesystem::path& root,
                                    const IgnoreFilter& filter,
                                    std::string& error) {
  std::vector<std::string> files;

  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    error = "cannot walk " + root.string() + ": " + ec.message();
    return files;
  }

  const std::filesystem::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) break;

    std::error_code entry_ec;
    const bool is_directory = it->is_directory(entry_ec);
    if (entry_ec) continue;

    if (filter.ignored(it->path().string(), is_directory)) {
      if (is_directory) it.disable_recursion_pending();
      continue;
    }

    if (is_directory) continue;
    if (not it->is_regular_file(entry_ec) or entry_ec) continue;

    const std::filesystem::path relative =
        std::filesystem::relative(it->path(), root, entry_ec);
    files.push_back(entry_ec ? it->path().generic_string()
                             : relative.generic_string());
  }

  return files;
}

std::string clip_line(const std::string& line) {
  if (line.size() <= kMaxLineWidth) return line;
  return line.substr(0, kMaxLineWidth) + "...";
}

}  // namespace

// ---------------------------------------------------------------------------

std::string FindTool::description() const {
  return R"json({"name":"find","description":"Find files by glob pattern, relative to search dir. Respects .gitignore. Truncated to 1000 results/100KB.","parameters":{"type":"object","properties":{"pattern":{"type":"string","description":"Glob pattern, e.g. '**/*.ts'"},"path":{"type":"string","description":"Search dir, default cwd"},"limit":{"type":"number","description":"Max results, default 1000"}},"required":["pattern"]}})json";
}

ToolResult FindTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> pattern = string_arg(args, "pattern");
  if (not pattern or pattern->empty()) {
    result.error = "find: missing required string argument 'pattern'";
    return result;
  }

  const std::string root = default_path(args);
  const int64_t limit = std::max<int64_t>(int_arg(args, "limit").value_or(1000), 1);

  std::regex matcher;
  try {
    matcher = std::regex(glob_to_regex(*pattern), std::regex::ECMAScript);
  } catch (const std::regex_error& e) {
    result.error = "find: bad glob pattern: " + std::string(e.what());
    return result;
  }

  const IgnoreFilter filter(root);
  std::string walk_error;
  std::vector<std::string> files = walk_files(root, filter, walk_error);
  if (not walk_error.empty()) {
    result.error = "find: " + walk_error;
    return result;
  }

  std::vector<std::string> matches;
  for (const std::string& file : files) {
    if (std::regex_match(file, matcher)) matches.push_back(file);
  }
  std::sort(matches.begin(), matches.end());

  const bool over_limit = static_cast<int64_t>(matches.size()) > limit;
  if (over_limit) matches.resize(static_cast<size_t>(limit));

  std::string output;
  for (const std::string& match : matches) {
    output += match;
    output += "\n";
  }

  TruncatedOutput truncated =
      truncate_output(std::move(output), "find", static_cast<size_t>(limit));

  result.ok = true;
  result.output = std::move(truncated.text);
  result.output += truncation_note(truncated);
  if (over_limit and not truncated.truncated) {
    result.output += "\n[results capped at " + std::to_string(limit) + "]";
  }
  result.truncated = truncated.truncated or over_limit;
  result.overflow_path = std::move(truncated.overflow_path);
  return result;
}

// ---------------------------------------------------------------------------

std::string GrepTool::description() const {
  return R"json({"name":"grep","description":"Search file contents by pattern (regex or literal). Respects .gitignore. Truncated to 100 matches/100KB; lines truncated to 2000 chars.","parameters":{"type":"object","properties":{"pattern":{"type":"string","description":"Regex or literal search pattern"},"path":{"type":"string","description":"Dir/file to search, default cwd"},"glob":{"type":"string","description":"Filter files by glob pattern"},"ignoreCase":{"type":"boolean","description":"Case-insensitive, default false"},"literal":{"type":"boolean","description":"Treat pattern as literal string, default false"},"context":{"type":"number","description":"Lines of context around match, default 0"},"limit":{"type":"number","description":"Max matches, default 100"}},"required":["pattern"]}})json";
}

ToolResult GrepTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> pattern = string_arg(args, "pattern");
  if (not pattern or pattern->empty()) {
    result.error = "grep: missing required string argument 'pattern'";
    return result;
  }

  const std::string root = default_path(args);
  const bool ignore_case = bool_arg(args, "ignoreCase").value_or(false);
  const bool literal = bool_arg(args, "literal").value_or(false);
  const int64_t context =
      std::max<int64_t>(int_arg(args, "context").value_or(0), 0);
  const int64_t limit =
      std::max<int64_t>(int_arg(args, "limit").value_or(100), 1);

  std::regex matcher;
  if (not literal) {
    auto flags = std::regex::ECMAScript;
    if (ignore_case) flags |= std::regex::icase;
    try {
      matcher = std::regex(*pattern, flags);
    } catch (const std::regex_error& e) {
      result.error = "grep: bad regex pattern: " + std::string(e.what());
      return result;
    }
  }

  std::string needle = *pattern;
  if (literal and ignore_case) {
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return std::tolower(c); });
  }

  const auto line_matches = [&](const std::string& line) {
    if (not literal) return std::regex_search(line, matcher);
    if (not ignore_case) return line.find(needle) != std::string::npos;
    std::string lowered = line;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lowered.find(needle) != std::string::npos;
  };

  // `path` may name a single file, in which case there's nothing to walk.
  std::error_code ec;
  const std::filesystem::path root_path(root);
  const bool root_is_file = std::filesystem::is_regular_file(root_path, ec);

  std::vector<std::string> files;
  std::filesystem::path base = root_path;
  if (root_is_file) {
    base = root_path.has_parent_path() ? root_path.parent_path()
                                       : std::filesystem::path(".");
    files.push_back(root_path.filename().string());
  } else {
    const IgnoreFilter filter(root);
    std::string walk_error;
    files = walk_files(root_path, filter, walk_error);
    if (not walk_error.empty()) {
      result.error = "grep: " + walk_error;
      return result;
    }
    std::sort(files.begin(), files.end());
  }

  if (const std::optional<std::string> glob = string_arg(args, "glob");
      glob and not glob->empty()) {
    std::regex glob_matcher;
    try {
      glob_matcher = std::regex(glob_to_regex(*glob), std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
      result.error = "grep: bad glob pattern: " + std::string(e.what());
      return result;
    }
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const std::string& file) {
                                 return not std::regex_match(file, glob_matcher);
                               }),
                files.end());
  }

  std::string output;
  int64_t match_count = 0;
  bool capped = false;

  for (const std::string& file : files) {
    if (capped) break;

    std::ifstream in(base / file, std::ios::binary);
    if (not in) continue;

    // Keep the last `context` lines around so a match can print what preceded
    // it without a second pass over the file.
    std::vector<std::string> before;
    std::string line;
    int64_t line_number = 0;
    int64_t trailing = 0;  // Context lines still owed after the last match.

    while (std::getline(in, line)) {
      ++line_number;
      if (not line.empty() and line.back() == '\r') line.pop_back();

      if (line_number == 1 and is_binary(line)) break;

      if (line_matches(line)) {
        for (size_t i = 0; i < before.size(); ++i) {
          const int64_t number =
              line_number - static_cast<int64_t>(before.size() - i);
          output += file + "-" + std::to_string(number) + "-" +
                    clip_line(before[i]) + "\n";
        }
        before.clear();
        output += file + ":" + std::to_string(line_number) + ":" +
                  clip_line(line) + "\n";
        trailing = context;
        if (++match_count >= limit) {
          capped = true;
          break;
        }
        continue;
      }

      if (trailing > 0) {
        output += file + "-" + std::to_string(line_number) + "-" +
                  clip_line(line) + "\n";
        --trailing;
        continue;
      }

      if (context > 0) {
        before.push_back(line);
        if (static_cast<int64_t>(before.size()) > context) before.erase(before.begin());
      }
    }
  }

  TruncatedOutput truncated = truncate_output(std::move(output), "grep");

  result.ok = true;
  result.output = std::move(truncated.text);
  result.output += truncation_note(truncated);
  if (capped and not truncated.truncated) {
    result.output += "\n[stopped at " + std::to_string(limit) + " matches]";
  }
  if (result.output.empty()) result.output = "[no matches]";
  result.truncated = truncated.truncated or capped;
  result.overflow_path = std::move(truncated.overflow_path);
  return result;
}

}  // namespace agent
