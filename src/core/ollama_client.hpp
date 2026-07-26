#ifndef OLLAMA_CLIENT_H
#define OLLAMA_CLIENT_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent {

struct ChatMessage {
  std::string role;
  std::string content;
};

struct ChatResult {
  bool ok = false;
  std::string content;
  std::string error;
};

struct GenerateOptions {
  // Maps onto /api/generate's own "options" sub-object.
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
  nlohmann::json format;            // string or JSON schema object; null = unset.
  std::string system;
  nlohmann::json think;             // bool or string; null = unset.
  std::optional<bool> raw;
  nlohmann::json keep_alive;        // string or number; null = unset.
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
  nlohmann::json keep_alive;  // string or number; null = unset.
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
  std::optional<GenerateOptions::ModelParams> model_params;  // Parsed from `parameters`; nullopt if show() failed.
  std::string license;
  std::string modified_at;
  std::vector<std::string> capabilities;
  ModelDetails details;
  std::string prompt_template;  // JSON key is "template" (a C++ keyword).
  nlohmann::json model_info;    // Arbitrary, architecture-dependent metadata.
  std::string error;
};

// Thin wrapper around ollama's HTTP API (https://github.com/ollama/ollama/blob/main/docs/api.md),
// talking to it over libcurl. One instance can be reused across calls and
// across threads.
struct OllamaClient {
  explicit OllamaClient(std::string host = "http://localhost:11434");

  ChatResult chat(std::string_view model,
                   const std::vector<ChatMessage>& messages) const;

  GenerateResult generate(std::string_view model, std::string_view prompt,
                           const GenerateOptions& options = {},
                           const std::optional<GenerateOptions::ModelParams>& default_params = std::nullopt) const;

  ShowResult show(std::string_view model, bool verbose = false) const;

  EmbedResult embed(std::string_view model,
                     const std::vector<std::string>& input,
                     const EmbedOptions& options = {},
                     const std::optional<GenerateOptions::ModelParams>& default_params = std::nullopt) const;

protected:
  // Internal result of one low-level POST. `ok` only means "HTTP transport
  // succeeded and status == 200" — each public method still does its own
  // endpoint-specific JSON parsing on top of this.
  struct HttpResult {
    bool ok = false;
    long status = 0;
    std::string body;
    std::string error;
  };

  HttpResult post_json(const std::string& path,
                        const std::string& payload) const;

  // Shared by generate() and embed(), whose "options" sub-object is the same
  // ModelParams shape: fills any field the caller left unset in `params`
  // from `defaults` (e.g. a prior show() call's parsed parameters).
  static GenerateOptions::ModelParams merge_model_params(
      GenerateOptions::ModelParams params,
      const std::optional<GenerateOptions::ModelParams>& defaults);
  static nlohmann::json model_params_to_json(
      const GenerateOptions::ModelParams& params);

private:
  std::string host_;
};

}  // namespace agent

#endif  // OLLAMA_CLIENT_H
