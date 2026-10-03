#ifndef M8_MCP_CONTENT_H
#define M8_MCP_CONTENT_H

#include <cstddef>
#include <string>
#include <vector>

#include <core/util/json_value.h>

namespace mcp {

// Turning what a server returns into text for the transcript.
//
// The model only reads text, so every content block becomes some: text as is,
// images, audio and binary resources saved to temp files and named by path (the
// agent's shell can then do something with them), links as links.
// Everything is UTF-8-repaired, because one bad byte reaching the transcript
// would empty every later request body (see util::sanitize_utf8), and capped,
// with the whole text saved beside the cut the way the shell tools do it.

struct RenderOptions {
  size_t max_bytes = 15000;   // beyond this the text spills to a temp file
  std::string label = "mcp";  // names the temp files
};

struct Rendered {
  std::string text;
  bool is_error = false;       // the tool reported failure (isError)
  bool truncated = false;
  std::string overflow_path;   // the full text, when truncated
  std::vector<std::string> saved_files;
};

// A CallToolResult: content[], structuredContent, isError.
Rendered render_tool_result(const util::JsonValue& result,
                            const RenderOptions& options = {});

// A ReadResourceResult: contents[] of text or blob entries.
Rendered render_resource_contents(const util::JsonValue& result,
                                  const RenderOptions& options = {});

// A GetPromptResult as the text of one user turn. A prompt that is all user
// messages reads as plain text; assistant turns are labelled.
std::string render_prompt_messages(const util::JsonValue& result);

}  // namespace mcp

#endif  // M8_MCP_CONTENT_H
