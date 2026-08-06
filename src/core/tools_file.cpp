#include <core/tools.h>

#include <cctype>
#include <cstdint>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <core/tools_util.h>

namespace agent {

namespace {

// The image formats read() advertises, mapped to the media type reported
// alongside the base64 payload.
std::string image_media_type(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (extension == ".jpg" or extension == ".jpeg") return "image/jpeg";
  if (extension == ".png") return "image/png";
  if (extension == ".gif") return "image/gif";
  if (extension == ".webp") return "image/webp";
  if (extension == ".bmp") return "image/bmp";
  return std::string();
}

std::string base64_encode(std::string_view bytes) {
  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string encoded;
  encoded.reserve(((bytes.size() + 2) / 3) * 4);

  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const uint32_t triple = (static_cast<unsigned char>(bytes[i]) << 16) |
                            (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                            static_cast<unsigned char>(bytes[i + 2]);
    encoded += kAlphabet[(triple >> 18) & 0x3F];
    encoded += kAlphabet[(triple >> 12) & 0x3F];
    encoded += kAlphabet[(triple >> 6) & 0x3F];
    encoded += kAlphabet[triple & 0x3F];
  }

  if (i < bytes.size()) {
    const bool has_two = (i + 1 < bytes.size());
    const uint32_t triple =
        (static_cast<unsigned char>(bytes[i]) << 16) |
        (has_two ? (static_cast<unsigned char>(bytes[i + 1]) << 8) : 0);
    encoded += kAlphabet[(triple >> 18) & 0x3F];
    encoded += kAlphabet[(triple >> 12) & 0x3F];
    encoded += has_two ? kAlphabet[(triple >> 6) & 0x3F] : '=';
    encoded += '=';
  }

  return encoded;
}

// The slice of `text` starting at line `offset` (1-indexed) and running for at
// most `limit` lines. A limit of 0 means "to the end".
std::string line_slice(const std::string& text, int64_t offset, int64_t limit) {
  if (offset <= 1 and limit <= 0) return text;

  std::istringstream stream(text);
  std::string line;
  std::string slice;
  int64_t line_number = 0;
  int64_t taken = 0;

  while (std::getline(stream, line)) {
    ++line_number;
    if (line_number < offset) continue;
    slice += line;
    slice += "\n";
    ++taken;
    if (limit > 0 and taken >= limit) break;
  }
  return slice;
}

// Where each edit's oldText sits in the original text, or an error naming the
// edit that couldn't be placed. Verifying every edit before applying any is
// what keeps a bad edit list from leaving a half-written file behind.
struct EditPlacement {
  size_t position = 0;
  size_t length = 0;
  const std::string* replacement = nullptr;
};

std::string place_edits(
    const std::string& text,
    const std::vector<std::pair<std::string, std::string>>& edits,
    std::vector<EditPlacement>& placements) {
  for (size_t i = 0; i < edits.size(); ++i) {
    const std::string& old_text = edits[i].first;
    const std::string label = "edits[" + std::to_string(i) + "]";

    if (old_text.empty()) {
      return label + ".oldText is empty";
    }

    const size_t first = text.find(old_text);
    if (first == std::string::npos) {
      return label + ".oldText was not found in the file";
    }
    if (text.find(old_text, first + 1) != std::string::npos) {
      return label + ".oldText is not unique in the file";
    }

    placements.push_back({first, old_text.size(), &edits[i].second});
  }

  std::sort(placements.begin(), placements.end(),
            [](const EditPlacement& a, const EditPlacement& b) {
              return a.position < b.position;
            });

  for (size_t i = 1; i < placements.size(); ++i) {
    if (placements[i].position < placements[i - 1].position +
                                     placements[i - 1].length) {
      return "edits overlap each other; merge them into one edit";
    }
  }

  return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------

std::string ReadTool::description() const {
  return R"json({"name":"read","description":"Read a file's contents. Supports text and images (jpg/png/gif/webp/bmp). Text output truncated to 5000 lines/100KB; use offset to continue.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path"},"offset":{"type":"number","description":"Start line, 1-indexed"},"limit":{"type":"number","description":"Max lines to read"}},"required":["path"]}})json";
}

ToolResult ReadTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> path = string_arg(args, "path");
  if (not path or path->empty()) {
    result.error = "read: missing required string argument 'path'";
    return result;
  }

  std::error_code ec;
  if (not std::filesystem::exists(*path, ec) or ec) {
    result.error = "read: no such file: " + *path;
    return result;
  }
  if (std::filesystem::is_directory(*path, ec)) {
    result.error = "read: path is a directory: " + *path;
    return result;
  }

  const std::optional<std::string> contents = read_file(*path);
  if (not contents) {
    result.error = "read: failed to open " + *path;
    return result;
  }

  const std::string media_type = image_media_type(*path);
  if (not media_type.empty()) {
    result.ok = true;
    result.output = "[" + media_type + "; base64]\n" + base64_encode(*contents);
    return result;
  }

  if (is_binary(std::string_view(*contents).substr(
          0, std::min<size_t>(contents->size(), 8192)))) {
    result.error = "read: " + *path + " looks like a binary file";
    return result;
  }

  const int64_t offset = int_arg(args, "offset").value_or(1);
  const int64_t limit = int_arg(args, "limit").value_or(0);

  TruncatedOutput truncated =
      truncate_output(line_slice(*contents, offset, limit), "read");

  result.ok = true;
  result.output = std::move(truncated.text);
  result.output += truncation_note(truncated);
  result.truncated = truncated.truncated;
  result.overflow_path = std::move(truncated.overflow_path);
  return result;
}

// ---------------------------------------------------------------------------

std::string WriteTool::description() const {
  return R"json({"name":"write","description":"Write content to a file, creating it and parent dirs if needed, overwriting if it exists.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path"},"content":{"type":"string","description":"File content"}},"required":["path","content"]}})json";
}

ToolResult WriteTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> path = string_arg(args, "path");
  if (not path or path->empty()) {
    result.error = "write: missing required string argument 'path'";
    return result;
  }
  const std::optional<std::string> content = string_arg(args, "content");
  if (not content) {
    result.error = "write: missing required string argument 'content'";
    return result;
  }

  const std::filesystem::path target(*path);
  if (target.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
      result.error = "write: failed to create " +
                     target.parent_path().string() + ": " + ec.message();
      return result;
    }
  }

  std::ofstream out(target, std::ios::binary | std::ios::trunc);
  if (not out) {
    result.error = "write: failed to open " + *path + " for writing";
    return result;
  }
  out.write(content->data(), static_cast<std::streamsize>(content->size()));
  if (not out.good()) {
    result.error = "write: failed while writing " + *path;
    return result;
  }

  result.ok = true;
  result.output = "wrote " + std::to_string(content->size()) + " bytes to " +
                  *path;
  return result;
}

// ---------------------------------------------------------------------------

std::string EditTool::description() const {
  return R"json({"name":"edit","description":"Edit a file via exact text replacement. Each edits[].oldText must be unique and non-overlapping in the file; merge nearby changes into one edit.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"File path"},"edits":{"type":"array","description":"Targeted replacements, matched against the original file","items":{"type":"object","properties":{"oldText":{"type":"string","description":"Exact unique text to replace"},"newText":{"type":"string","description":"Replacement text"}},"required":["oldText","newText"]}}},"required":["path","edits"]}})json";
}

ToolResult EditTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> path = string_arg(args, "path");
  if (not path or path->empty()) {
    result.error = "edit: missing required string argument 'path'";
    return result;
  }

  const std::vector<std::pair<std::string, std::string>>* edits =
      pairs_arg(args, "edits");
  if (edits == nullptr or edits->empty()) {
    result.error =
        "edit: missing required argument 'edits' (array of {oldText, newText})";
    return result;
  }

  const std::optional<std::string> contents = read_file(*path);
  if (not contents) {
    result.error = "edit: failed to read " + *path;
    return result;
  }

  std::vector<EditPlacement> placements;
  placements.reserve(edits->size());
  const std::string placement_error = place_edits(*contents, *edits, placements);
  if (not placement_error.empty()) {
    // Nothing has been written yet, so the file is untouched.
    result.error = "edit: " + placement_error;
    return result;
  }

  std::string edited;
  edited.reserve(contents->size());
  size_t copied = 0;
  for (const EditPlacement& placement : placements) {
    edited.append(*contents, copied, placement.position - copied);
    edited.append(*placement.replacement);
    copied = placement.position + placement.length;
  }
  edited.append(*contents, copied, std::string::npos);

  std::ofstream out(*path, std::ios::binary | std::ios::trunc);
  if (not out) {
    result.error = "edit: failed to open " + *path + " for writing";
    return result;
  }
  out.write(edited.data(), static_cast<std::streamsize>(edited.size()));
  if (not out.good()) {
    result.error = "edit: failed while writing " + *path;
    return result;
  }

  result.ok = true;
  result.output = "applied " + std::to_string(edits->size()) + " edit(s) to " +
                  *path;
  return result;
}

}  // namespace agent
