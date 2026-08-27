#ifndef OLLAMA_CLIENT_H
#define OLLAMA_CLIENT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <core/json_util.h>

namespace agent {

// One tool invocation the model asked for. `arguments` is kept as the verbatim
// JSON object text rather than a parsed structure: the shape is per-tool, and
// agent::args_from_json() in core/tools_util.h is the one place that turns it
// into a ToolArgs map.
struct ToolCall {
  std::string name;
  std::string arguments;
};

struct ChatMessage {
  std::string role;  // "system" | "user" | "assistant" | "tool"
  std::string content;
  // Set on an assistant turn that asked for tools. Replayed on the way back
  // out, since ollama needs the request that a "tool" message answers.
  std::vector<ToolCall> tool_calls;
  // Which tool produced `content`. Only meaningful when role == "tool".
  std::string tool_name;
};

struct ChatResult {
  bool ok = false;
  std::string content;
  // Non-empty when the model wants tools run before it answers. `content` is
  // usually empty in that case, and that is not an error.
  std::vector<ToolCall> tool_calls;
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
  std::optional<GenerateOptions::ModelParams> model_params;  // Parsed from `parameters`; nullopt if show() failed.
  std::string license;
  std::string modified_at;
  std::vector<std::string> capabilities;
  ModelDetails details;
  std::string prompt_template;  // JSON key is "template" (a C++ keyword).
  RawJson model_info;           // Arbitrary, architecture-dependent metadata, as JSON text.
  std::string error;
};

// Thin wrapper around ollama's HTTP API (https://github.com/ollama/ollama/blob/main/docs/api.md),
// talking to it over libcurl. One global instance is accessed via instance().
// configure() must be called once before enqueue_chat() to set the model (and
// optionally the host).
struct OllamaClient {
  // Singleton access. The instance is created on first call.
  static OllamaClient& instance();

  // Set the model used by enqueue_chat() and (optionally) the host.
  // Must be called before any enqueue_chat() call.
  static void configure(const std::string& model,
                        const std::string& host = "http://localhost:11434");

  // Enqueues a chat request. Returns immediately with a ticket number.
  // The worker thread picks jobs in FIFO order, serialising calls to Ollama.
  uint64_t enqueue_chat(const std::vector<ChatMessage>& messages,
                        const std::vector<std::string>& tools = {});

  // Blocks until the job identified by `ticket` completes and returns its
  // result. Each ticket may only be waited on once.
  ChatResult wait_for(uint64_t ticket);

  ~OllamaClient();

  // `tools` holds each tool's schema as JSON object text — exactly what the
  // tool classes' description() returns. They are wrapped as
  // {"type":"function","function":<schema>} on the way out, which is the shape
  // /api/chat expects. An empty `tools` sends no tools array at all.
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
  // Serialized as a JSON object; empty string when no field is set, so the
  // caller can omit the "options" key entirely.
  static std::string model_params_to_json(
      const GenerateOptions::ModelParams& params);

private:
  OllamaClient();  // Starts the worker thread; use instance() for access.

  void worker_loop();

  std::string mHost;
  std::string mModel;

  struct Job {
    uint64_t ticket;
    std::vector<ChatMessage> messages;
    std::vector<std::string> tools;
    std::promise<ChatResult> promise;
  };

  std::deque<Job> mQueue;
  std::mutex mQueueMutex;
  std::condition_variable mQueueCv;
  std::atomic<uint64_t> mNextTicket{0};
  std::unordered_map<uint64_t, std::future<ChatResult>> mResults;
  std::mutex mResultsMutex;
  std::thread mWorker;
  bool mShutdown{false};
};

}  // namespace agent

#endif  // OLLAMA_CLIENT_H
