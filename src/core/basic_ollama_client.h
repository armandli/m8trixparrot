#ifndef BASIC_OLLAMA_CLIENT_H
#define BASIC_OLLAMA_CLIENT_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/json_util.h>

namespace agent {

struct ToolCall {
  std::string name;
  std::string arguments;
};

struct ChatMessage {
  std::string role;  // "system" | "user" | "assistant" | "tool"
  std::string content;
  std::vector<ToolCall> tool_calls;
  std::string tool_name;  // Only meaningful when role == "tool".
};

struct ChatResult {
  bool ok = false;
  std::string content;
  std::vector<ToolCall> tool_calls;
  std::string error;
};

struct GenerateOptions {
  struct ModelParams {
    std::optional<int64_t> seed;
    std::optional<double> temperature;
    std::optional<int64_t> top_k;
    std::optional<double> top_p;
    std::optional<double> min_p;
    std::vector<std::string> stop;
    std::optional<int64_t> num_ctx;
    std::optional<int64_t> num_predict;
  };

  std::string suffix;
  std::vector<std::string> images;  // Pre-base64-encoded by the caller.
  RawJson format;                   // string or JSON schema object; empty = unset.
  std::string system;
  RawJson think;                    // bool or string; empty = unset.
  std::optional<bool> raw;
  RawJson keep_alive;               // string or number; empty = unset.
  ModelParams model_params;
  std::optional<bool> logprobs;
  std::optional<int64_t> top_logprobs;
};

struct GenerateResult {
  bool ok = false;
  std::string content;  // The response's "response" field (generated text).
  std::string error;
};

struct EmbedOptions {
  std::optional<bool> truncate;  // API default is true; unset omits the field.
  std::optional<int64_t> dimensions;
  RawJson keep_alive;         // string or number; empty = unset.
  GenerateOptions::ModelParams model_params;
};

struct EmbedResult {
  bool ok = false;
  std::string model;
  std::vector<std::vector<double>> embeddings;
  int64_t total_duration = 0;
  int64_t load_duration = 0;
  int64_t prompt_eval_count = 0;
  std::string error;
};

struct ModelDetails {
  std::string parent_model;
  std::string format;
  std::string family;
  std::vector<std::string> families;
  std::string parameter_size;
  std::string quantization_level;
};

struct ShowResult {
  bool ok = false;
  std::string parameters;
  std::optional<GenerateOptions::ModelParams> model_params;  // Parsed from `parameters`.
  std::string license;
  std::string modified_at;
  std::vector<std::string> capabilities;
  ModelDetails details;
  std::string prompt_template;  // JSON key is "template" (a C++ keyword).
  RawJson model_info;
  std::string error;
};

// Synchronous, non-singleton HTTP client for the Ollama API. Owns a host
// string; all methods take the model name explicitly. Not thread-safe for
// concurrent calls on the same instance — create one per thread if needed.
struct BasicOllamaClient {
  explicit BasicOllamaClient(
      const std::string& host = "http://localhost:11434");

  // `tools` holds each tool's schema as JSON object text. Wrapped as
  // {"type":"function","function":<schema>} on the way out. Empty = no tools.
  ChatResult chat(std::string_view model,
                   const std::vector<ChatMessage>& messages,
                   const std::vector<std::string>& tools = {}) const;

  GenerateResult generate(std::string_view model, std::string_view prompt,
                           const GenerateOptions& options = {},
                           const std::optional<GenerateOptions::ModelParams>& default_params = std::nullopt) const;

  ShowResult show(std::string_view model, bool verbose = false) const;

  EmbedResult embed(std::string_view model,
                     const std::vector<std::string>& input,
                     const EmbedOptions& options = {},
                     const std::optional<GenerateOptions::ModelParams>& default_params = std::nullopt) const;

protected:
  struct HttpResult {
    bool ok = false;
    long status = 0;
    std::string body;
    std::string error;
  };

  HttpResult post_json(const std::string& path,
                        const std::string& payload) const;

  static GenerateOptions::ModelParams merge_model_params(
      GenerateOptions::ModelParams params,
      const std::optional<GenerateOptions::ModelParams>& defaults);

  static std::string model_params_to_json(
      const GenerateOptions::ModelParams& params);

public:
  std::string mHost;
};

}  // namespace agent

#endif  // BASIC_OLLAMA_CLIENT_H
