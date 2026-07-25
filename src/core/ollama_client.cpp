#include "core/ollama_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace agent {

namespace {

size_t WriteCallback(char* data, size_t size, size_t nmemb, void* userp) {
  auto* out = static_cast<std::string*>(userp);
  out->append(data, size * nmemb);
  return size * nmemb;
}

// curl_global_init() must run once before any curl handle is used, and is
// not itself thread-safe, so it happens here via a function-local static
// (C++11 guarantees the initializer runs exactly once, safely).
void EnsureCurlInitialized() {
  static const CURLcode init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  (void)init_result;
}

}  // namespace

OllamaClient::OllamaClient(std::string host) : host_(std::move(host)) {
  EnsureCurlInitialized();
}

ChatResult OllamaClient::Chat(const std::string& model,
                               const std::vector<ChatMessage>& messages) const {
  ChatResult result;

  nlohmann::json body;
  body["model"] = model;
  body["stream"] = false;
  body["messages"] = nlohmann::json::array();
  for (const auto& message : messages) {
    body["messages"].push_back({{"role", message.role}, {"content", message.content}});
  }
  const std::string payload = body.dump();

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    result.error = "failed to initialize curl handle";
    return result;
  }

  std::string response;
  const std::string url = host_ + "/api/chat";

  curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);

  const CURLcode code = curl_easy_perform(curl);
  curl_slist_free_all(headers);

  if (code != CURLE_OK) {
    result.error = curl_easy_strerror(code);
    curl_easy_cleanup(curl);
    return result;
  }

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);

  if (status != 200) {
    result.error = "ollama returned HTTP " + std::to_string(status) + ": " + response;
    return result;
  }

  try {
    const auto parsed = nlohmann::json::parse(response);
    result.content = parsed.at("message").at("content").get<std::string>();
    result.ok = true;
  } catch (const std::exception& e) {
    result.error = std::string("failed to parse ollama response: ") + e.what();
  }

  return result;
}

}  // namespace agent
