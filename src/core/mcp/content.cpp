#include <core/mcp/content.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>

#include <core/tools/tools_util.h>
#include <core/util/base64.h>
#include <core/util/text.h>
#include <core/util/uuid.h>

namespace mcp {

namespace {

using util::JsonValue;

std::string extension_for(const std::string& mime) {
  static const std::pair<const char*, const char*> kKnown[] = {
      {"image/png", ".png"},         {"image/jpeg", ".jpg"},
      {"image/jpg", ".jpg"},         {"image/gif", ".gif"},
      {"image/webp", ".webp"},       {"image/svg+xml", ".svg"},
      {"audio/wav", ".wav"},         {"audio/x-wav", ".wav"},
      {"audio/mpeg", ".mp3"},        {"audio/ogg", ".ogg"},
      {"application/pdf", ".pdf"},   {"application/json", ".json"},
      {"text/plain", ".txt"},        {"application/zip", ".zip"},
  };
  for (const auto& [type, extension] : kKnown) {
    if (mime == type) return extension;
  }
  return ".bin";
}

std::string human_size(size_t bytes) {
  char buf[32];
  if (bytes < 1024) {
    std::snprintf(buf, sizeof(buf), "%zu B", bytes);
  } else if (bytes < 1024 * 1024) {
    std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
  }
  return buf;
}

// Decodes base64 `data` and writes it to a fresh temp file. The path, or
// nullopt when the payload is not base64 or the write failed.
std::optional<std::string> save_blob(const std::string& data,
                                     const std::string& mime,
                                     const RenderOptions& options,
                                     size_t& size) {
  std::optional<std::string> bytes = util::base64_decode(data);
  if (not bytes) return std::nullopt;
  size = bytes->size();

  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
  if (ec) return std::nullopt;
  const std::filesystem::path path =
      dir / ("m8-" + options.label + "-" + util::generate_uuid_v4().substr(0, 8) +
             extension_for(mime));
  std::ofstream out(path, std::ios::binary);
  if (not out) return std::nullopt;
  out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
  if (not out.good()) return std::nullopt;
  return path.string();
}

std::string render_binary(const std::string& kind, const std::string& data,
                          const std::string& mime, const RenderOptions& options,
                          Rendered& rendered, const std::string& uri = "") {
  std::string label = "[" + kind;
  if (not uri.empty()) label += " " + uri;
  label += " " + (mime.empty() ? std::string("application/octet-stream") : mime);
  size_t size = 0;
  if (std::optional<std::string> path = save_blob(data, mime, options, size)) {
    rendered.saved_files.push_back(*path);
    return label + ", " + human_size(size) + ", saved to " + *path + "]";
  }
  return label + ", not decodable]";
}

// An embedded resource or a resources/read entry.
std::string render_resource(const JsonValue& resource,
                            const RenderOptions& options, Rendered& rendered) {
  const std::string& uri = resource.get("uri").as_string();
  const std::string& mime = resource.get("mimeType").as_string();
  if (const JsonValue* text = resource.find("text"); text and text->is_string()) {
    std::string header = "--- " + uri;
    if (not mime.empty()) header += " (" + mime + ")";
    return header + " ---\n" + text->as_string();
  }
  if (const JsonValue* blob = resource.find("blob"); blob and blob->is_string()) {
    return render_binary("resource", blob->as_string(), mime, options, rendered,
                         uri);
  }
  return "[resource " + uri + " with no contents]";
}

std::string render_block(const JsonValue& block, const RenderOptions& options,
                         Rendered& rendered, bool& was_text) {
  const std::string& type = block.get("type").as_string();
  was_text = false;
  if (type == "text") {
    was_text = true;
    return block.get("text").as_string();
  }
  if (type == "image" or type == "audio") {
    return render_binary(type, block.get("data").as_string(),
                         block.get("mimeType").as_string(), options, rendered);
  }
  if (type == "resource_link") {
    std::string out = "[resource link] ";
    const std::string& name = block.get("name").as_string();
    if (not name.empty()) out += name + " — ";
    out += block.get("uri").as_string();
    if (const std::string& mime = block.get("mimeType").as_string();
        not mime.empty()) {
      out += " (" + mime + ")";
    }
    if (const std::string& description = block.get("description").as_string();
        not description.empty()) {
      out += "\n  " + description;
    }
    return out;
  }
  if (type == "resource") {
    was_text = true;
    return render_resource(block.get("resource"), options, rendered);
  }
  return "[content of type '" + type + "' omitted]";
}

void finish(Rendered& rendered, std::string text, const RenderOptions& options) {
  text = util::sanitize_utf8(text);
  tools::TruncatedOutput cut =
      tools::truncate_output(std::move(text), options.label,
                             tools::kMaxOutputLines, options.max_bytes);
  rendered.truncated = cut.truncated;
  rendered.overflow_path = cut.overflow_path;
  rendered.text = cut.text + tools::truncation_note(cut);
}

}  // namespace

Rendered render_tool_result(const util::JsonValue& result,
                            const RenderOptions& options) {
  Rendered rendered;
  rendered.is_error = result.get("isError").as_bool(false);

  std::string text;
  bool any_text = false;
  for (const JsonValue& block : result.get("content").items()) {
    bool was_text = false;
    const std::string part = render_block(block, options, rendered, was_text);
    any_text = any_text or was_text;
    if (not text.empty()) text += "\n";
    text += part;
  }

  // Servers SHOULD mirror structuredContent into a text block; when one does
  // not, the structured value is all there is.
  if (const JsonValue* structured = result.find("structuredContent");
      structured != nullptr and not structured->is_null() and not any_text) {
    if (not text.empty()) text += "\n";
    text += structured->dump_pretty();
  }

  finish(rendered, std::move(text), options);
  return rendered;
}

Rendered render_resource_contents(const util::JsonValue& result,
                                  const RenderOptions& options) {
  Rendered rendered;
  std::string text;
  for (const JsonValue& entry : result.get("contents").items()) {
    if (not text.empty()) text += "\n";
    text += render_resource(entry, options, rendered);
  }
  if (text.empty()) text = "[the resource has no contents]";
  finish(rendered, std::move(text), options);
  return rendered;
}

std::string render_prompt_messages(const util::JsonValue& result) {
  const std::vector<JsonValue>& messages = result.get("messages").items();
  bool only_user = true;
  for (const JsonValue& message : messages) {
    only_user = only_user and message.get("role").as_string() != "assistant";
  }

  RenderOptions options;
  options.label = "mcp-prompt";
  options.max_bytes = 1u << 20;
  Rendered scratch;

  std::string text;
  for (const JsonValue& message : messages) {
    bool was_text = false;
    const std::string part =
        render_block(message.get("content"), options, scratch, was_text);
    if (not text.empty()) text += "\n\n";
    if (not only_user) text += "[" + message.get("role").string_or("user") + "]\n";
    text += part;
  }
  return util::sanitize_utf8(text);
}

}  // namespace mcp
