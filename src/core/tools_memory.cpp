#include <core/tools.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include <core/tools_util.h>

namespace agent {

std::string MemoryTool::description() const {
  return R"json({"name":"memory","description":"Replace your working memory notes, which are shown back to you at the start of every turn. Record the current task, decisions made, files changed and anything you must not forget; rewrite the whole thing each time.","parameters":{"type":"object","properties":{"content":{"type":"string","description":"The full memory content, in markdown"}},"required":["content"]}})json";
}

ToolResult MemoryTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> content = string_arg(args, "content");
  if (not content) {
    result.error = "memory: missing required string argument 'content'";
    return result;
  }

  const std::filesystem::path path(kMemoryPath);
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      result.error = "memory: failed to create " + path.parent_path().string() +
                     ": " + ec.message();
      return result;
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (not out) {
    result.error = "memory: failed to open " + std::string(kMemoryPath) +
                   " for writing";
    return result;
  }
  out.write(content->data(), static_cast<std::streamsize>(content->size()));
  if (not out.good()) {
    result.error = "memory: failed while writing " + std::string(kMemoryPath);
    return result;
  }

  result.ok = true;
  result.output = "memory updated (" + std::to_string(content->size()) +
                  " bytes)";
  return result;
}

}  // namespace agent
