#include <core/ollama_client.hpp>

#include <sstream>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace agent {

namespace {

size_t write_callback(char* data, size_t size, size_t nmemb, void* userp) {
  auto* out = static_cast<std::string*>(userp);
  out->append(data, size * nmemb);
  return size * nmemb;
}

std::string strip_quotes(std::string_view value) {
  if (value.size() >= 2 and value.front() == '"' and value.back() == '"') {
    return std::string(value.substr(1, value.size() - 2));
  }
  return std::string(value);
}

// Parses ollama's Modelfile-style "PARAMETER key value" text (the /api/show
// response's "parameters" field) into the subset of fields generate() cares
// about. Unrecognized keys and malformed values are skipped rather than
// failing the whole parse.
GenerateOptions::ModelParams parse_model_params(const std::string& raw) {
  GenerateOptions::ModelParams params;

  std::istringstream stream(raw);
  std::string line;
  while (std::getline(stream, line)) {
    std::istringstream line_stream(line);
    std::string key;
    if (not (line_stream >> key)) continue;

    std::string value;
    std::getline(line_stream, value);
    const size_t start = value.find_first_not_of(" \t");
    if (start == std::string::npos) continue;
    value = value.substr(start);

    try {
      if (key == "seed") {
        params.seed = std::stoll(value);
      } else if (key == "temperature") {
        params.temperature = std::stod(value);
      } else if (key == "top_k") {
        params.top_k = std::stoll(value);
      } else if (key == "top_p") {
        params.top_p = std::stod(value);
      } else if (key == "min_p") {
        params.min_p = std::stod(value);
      } else if (key == "stop") {
        params.stop.push_back(strip_quotes(value));
      } else if (key == "num_ctx") {
        params.num_ctx = std::stoll(value);
      } else if (key == "num_predict") {
        params.num_predict = std::stoll(value);
      }
    } catch (const std::exception&) {
      // Malformed value for a recognized key; skip this line.
    }
  }

  return params;
}

// curl_global_init() must run once before any curl handle is used, and is
// not itself thread-safe, so it happens here via a function-local static
// (C++11 guarantees the initializer runs exactly once, safely).
void ensure_curl_initialized() {
  static const CURLcode init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  (void)init_result;
}

}  // namespace

OllamaClient::OllamaClient(std::string host) : host_(std::move(host)) {
  ensure_curl_initialized();
}

ChatResult OllamaClient::chat(std::string_view model,
                               const std::vector<ChatMessage>& messages) const {
  ChatResult result;

  nlohmann::json body;
  body["model"] = model;
  body["stream"] = false;
  body["messages"] = nlohmann::json::array();
  for (const auto& message : messages) {
    body["messages"].push_back(
        {{"role", message.role}, {"content", message.content}});
  }

  const HttpResult http = post_json("/api/chat", body.dump());
  if (not http.ok) {
    result.error = http.error;
    return result;
  }

  try {
    const auto parsed = nlohmann::json::parse(http.body);
    result.content = parsed.at("message").at("content").get<std::string>();
    result.ok = true;
  } catch (const std::exception& e) {
    result.error = std::string("failed to parse ollama response: ") + e.what();
  }

  return result;
}

GenerateOptions::ModelParams OllamaClient::merge_model_params(
    GenerateOptions::ModelParams params,
    const std::optional<GenerateOptions::ModelParams>& defaults) {
  if (defaults) {
    if (not params.seed) params.seed = defaults->seed;
    if (not params.temperature) params.temperature = defaults->temperature;
    if (not params.top_k) params.top_k = defaults->top_k;
    if (not params.top_p) params.top_p = defaults->top_p;
    if (not params.min_p) params.min_p = defaults->min_p;
    if (params.stop.empty()) params.stop = defaults->stop;
    if (not params.num_ctx) params.num_ctx = defaults->num_ctx;
    if (not params.num_predict) params.num_predict = defaults->num_predict;
  }
  return params;
}

nlohmann::json OllamaClient::model_params_to_json(
    const GenerateOptions::ModelParams& params) {
  nlohmann::json model_params;
  if (params.seed) model_params["seed"] = *params.seed;
  if (params.temperature) model_params["temperature"] = *params.temperature;
  if (params.top_k) model_params["top_k"] = *params.top_k;
  if (params.top_p) model_params["top_p"] = *params.top_p;
  if (params.min_p) model_params["min_p"] = *params.min_p;
  if (not params.stop.empty()) model_params["stop"] = params.stop;
  if (params.num_ctx) model_params["num_ctx"] = *params.num_ctx;
  if (params.num_predict) model_params["num_predict"] = *params.num_predict;
  return model_params;
}

GenerateResult OllamaClient::generate(std::string_view model,
                                       std::string_view prompt,
                                       const GenerateOptions& options,
                                       const std::optional<GenerateOptions::ModelParams>& default_params) const {
  GenerateResult result;

  nlohmann::json body;
  body["model"] = model;
  body["prompt"] = prompt;
  body["stream"] = false;

  if (not options.suffix.empty()) body["suffix"] = options.suffix;
  if (not options.images.empty()) body["images"] = options.images;
  if (not options.format.is_null()) body["format"] = options.format;
  if (not options.system.empty()) body["system"] = options.system;
  if (not options.think.is_null()) body["think"] = options.think;
  if (options.raw) body["raw"] = *options.raw;
  if (not options.keep_alive.is_null()) body["keep_alive"] = options.keep_alive;
  if (options.logprobs) body["logprobs"] = *options.logprobs;
  if (options.top_logprobs) body["top_logprobs"] = *options.top_logprobs;

  const GenerateOptions::ModelParams params =
      merge_model_params(options.model_params, default_params);
  const nlohmann::json model_params = model_params_to_json(params);
  if (not model_params.empty()) body["options"] = model_params;

  const HttpResult http = post_json("/api/generate", body.dump());
  if (not http.ok) {
    result.error = http.error;
    return result;
  }

  try {
    const auto parsed = nlohmann::json::parse(http.body);
    result.content = parsed.at("response").get<std::string>();
    result.ok = true;
  } catch (const std::exception& e) {
    result.error = std::string("failed to parse ollama response: ") + e.what();
  }

  return result;
}

EmbedResult OllamaClient::embed(std::string_view model,
                                 const std::vector<std::string>& input,
                                 const EmbedOptions& options,
                                 const std::optional<GenerateOptions::ModelParams>& default_params) const {
  EmbedResult result;

  nlohmann::json body;
  body["model"] = model;
  body["input"] = input;
  if (options.truncate) body["truncate"] = *options.truncate;
  if (options.dimensions) body["dimensions"] = *options.dimensions;
  if (not options.keep_alive.is_null()) body["keep_alive"] = options.keep_alive;

  const GenerateOptions::ModelParams params =
      merge_model_params(options.model_params, default_params);
  const nlohmann::json model_params = model_params_to_json(params);
  if (not model_params.empty()) body["options"] = model_params;

  const HttpResult http = post_json("/api/embed", body.dump());
  if (not http.ok) {
    result.error = http.error;
    return result;
  }

  try {
    const auto parsed = nlohmann::json::parse(http.body);
    result.model = parsed.value("model", "");
    if (parsed.contains("embeddings")) {
      result.embeddings =
          parsed.at("embeddings").get<std::vector<std::vector<double>>>();
    }
    result.total_duration = parsed.value("total_duration", int64_t{0});
    result.load_duration = parsed.value("load_duration", int64_t{0});
    result.prompt_eval_count = parsed.value("prompt_eval_count", int64_t{0});
    result.ok = true;
  } catch (const std::exception& e) {
    result.error = std::string("failed to parse ollama response: ") + e.what();
  }

  return result;
}

ShowResult OllamaClient::show(std::string_view model, bool verbose) const {
  ShowResult result;

  nlohmann::json body;
  body["model"] = model;
  if (verbose) body["verbose"] = true;

  const HttpResult http = post_json("/api/show", body.dump());
  if (not http.ok) {
    result.error = http.error;
    return result;
  }

  try {
    const auto parsed = nlohmann::json::parse(http.body);
    result.parameters = parsed.value("parameters", "");
    result.model_params = parse_model_params(result.parameters);
    result.license = parsed.value("license", "");
    result.modified_at = parsed.value("modified_at", "");
    if (parsed.contains("capabilities")) {
      result.capabilities =
          parsed.at("capabilities").get<std::vector<std::string>>();
    }
    if (parsed.contains("details")) {
      const auto& details = parsed.at("details");
      result.details.parent_model = details.value("parent_model", "");
      result.details.format = details.value("format", "");
      result.details.family = details.value("family", "");
      if (details.contains("families") and
          not details.at("families").is_null()) {
        result.details.families =
            details.at("families").get<std::vector<std::string>>();
      }
      result.details.parameter_size = details.value("parameter_size", "");
      result.details.quantization_level =
          details.value("quantization_level", "");
    }
    result.prompt_template = parsed.value("template", "");
    if (parsed.contains("model_info")) {
      result.model_info = parsed.at("model_info");
    }
    result.ok = true;
  } catch (const std::exception& e) {
    result.error = std::string("failed to parse ollama response: ") + e.what();
  }

  return result;
}

OllamaClient::HttpResult OllamaClient::post_json(
    const std::string& path, const std::string& payload) const {
  HttpResult result;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    result.error = "failed to initialize curl handle";
    return result;
  }

  std::string response;
  const std::string url = host_ + path;
  curl_slist* headers =
      curl_slist_append(nullptr, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                    static_cast<long>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
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

  result.status = status;
  result.body = std::move(response);

  if (status != 200) {
    result.error =
        "ollama returned HTTP " + std::to_string(status) + ": " + result.body;
    return result;
  }

  result.ok = true;
  return result;
}

}  // namespace agent
