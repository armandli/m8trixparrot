#pragma once

#include <string>
#include <vector>

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

// Thin wrapper around ollama's HTTP API (https://github.com/ollama/ollama/blob/main/docs/api.md),
// talking to it over libcurl. One instance can be reused across calls and
// across threads.
class OllamaClient {
 public:
  explicit OllamaClient(std::string host = "http://localhost:11434");

  ChatResult Chat(const std::string& model,
                   const std::vector<ChatMessage>& messages) const;

 private:
  std::string host_;
};

}  // namespace agent
