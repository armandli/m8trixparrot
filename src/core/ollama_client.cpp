#include <core/ollama_client.hpp>

#include <sstream>

#include <curl/curl.h>
#include <simdjson.h>

#include <core/json_util.hpp>

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

// Parses `body` and hands the top-level object to `extract`. simdjson's
// On-Demand cursor, the document and the padded copy of the body all have to
// outlive the field accesses, so they live here rather than at each call site.
// Returns an error string; empty means success.
template <typename Extract>
std::string with_parsed_object(const std::string& body, Extract extract) {
  try {
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(body);
    simdjson::ondemand::document doc = parser.iterate(padded);

    simdjson::ondemand::object obj;
    if (auto error = doc.get_object().get(obj)) {
      return std::string("failed to parse ollama response: ") +
             simdjson::error_message(error);
    }
    extract(obj);
    return std::string();
  } catch (const std::exception& e) {
    return std::string("failed to parse ollama response: ") + e.what();
  }
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
                               const std::vector<ChatMessage>& messages,
                               const std::vector<std::string>& tools) const {
  ChatResult result;

  JsonWriter body;
  body.begin_object();
  body.field("model", model);
  body.field("stream", false);
  body.key("messages").begin_array();
  for (const auto& message : messages) {
    body.begin_object();
    body.field("role", message.role);
    body.field("content", message.content);
    if (not message.tool_name.empty()) {
      body.field("tool_name", message.tool_name);
    }
    if (not message.tool_calls.empty()) {
      body.key("tool_calls").begin_array();
      for (const auto& call : message.tool_calls) {
        body.begin_object();
        body.key("function").begin_object();
        body.field("name", call.name);
        // Already JSON object text; splice it in rather than re-escaping.
        body.field("arguments", RawJson::of_raw(call.arguments.empty()
                                                     ? "{}"
                                                     : call.arguments));
        body.end_object();
        body.end_object();
      }
      body.end_array();
    }
    body.end_object();
  }
  body.end_array();
  if (not tools.empty()) {
    body.key("tools").begin_array();
    for (const auto& schema : tools) {
      body.begin_object();
      body.field("type", "function");
      body.field("function", RawJson::of_raw(schema));
      body.end_object();
    }
    body.end_array();
  }
  body.end_object();

  const HttpResult http = post_json("/api/chat", body.str());
  if (not http.ok) {
    result.error = http.error;
    return result;
  }

  bool found_message = false;
  result.error = with_parsed_object(http.body, [&](simdjson::ondemand::object& obj) {
    simdjson::ondemand::object message;
    if (obj["message"].get_object().get(message)) return;
    found_message = true;

    result.content = string_field(message, "content");

    simdjson::ondemand::array calls;
    if (message["tool_calls"].get_array().get(calls)) return;
    for (auto element : calls) {
      simdjson::ondemand::object call;
      if (element.get_object().get(call)) continue;
      simdjson::ondemand::object function;
      if (call["function"].get_object().get(function)) continue;

      ToolCall tool_call;
      tool_call.name = string_field(function, "name");
      tool_call.arguments = raw_field(function, "arguments", "{}");
      if (tool_call.name.empty()) continue;
      result.tool_calls.push_back(std::move(tool_call));
    }
  });

  if (result.error.empty()) {
    // A turn that only asks for tools has empty content, and that is a valid
    // reply — the message object being present is what matters.
    if (found_message) {
      result.ok = true;
    } else {
      result.error = "failed to parse ollama response: missing message";
    }
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

std::string OllamaClient::model_params_to_json(
    const GenerateOptions::ModelParams& params) {
  const bool any = params.seed or params.temperature or params.top_k or
                   params.top_p or params.min_p or not params.stop.empty() or
                   params.num_ctx or params.num_predict;
  if (not any) return std::string();

  JsonWriter model_params;
  model_params.begin_object();
  if (params.seed) model_params.field("seed", *params.seed);
  if (params.temperature) model_params.field("temperature", *params.temperature);
  if (params.top_k) model_params.field("top_k", *params.top_k);
  if (params.top_p) model_params.field("top_p", *params.top_p);
  if (params.min_p) model_params.field("min_p", *params.min_p);
  if (not params.stop.empty()) model_params.field("stop", params.stop);
  if (params.num_ctx) model_params.field("num_ctx", *params.num_ctx);
  if (params.num_predict) model_params.field("num_predict", *params.num_predict);
  model_params.end_object();
  return model_params.str();
}

GenerateResult OllamaClient::generate(std::string_view model,
                                       std::string_view prompt,
                                       const GenerateOptions& options,
                                       const std::optional<GenerateOptions::ModelParams>& default_params) const {
  GenerateResult result;

  JsonWriter body;
  body.begin_object();
  body.field("model", model);
  body.field("prompt", prompt);
  body.field("stream", false);

  if (not options.suffix.empty()) body.field("suffix", options.suffix);
  if (not options.images.empty()) body.field("images", options.images);
  if (not options.format.empty()) body.field("format", options.format);
  if (not options.system.empty()) body.field("system", options.system);
  if (not options.think.empty()) body.field("think", options.think);
  if (options.raw) body.field("raw", *options.raw);
  if (not options.keep_alive.empty()) body.field("keep_alive", options.keep_alive);
  if (options.logprobs) body.field("logprobs", *options.logprobs);
  if (options.top_logprobs) body.field("top_logprobs", *options.top_logprobs);

  const GenerateOptions::ModelParams params =
      merge_model_params(options.model_params, default_params);
  const std::string model_params = model_params_to_json(params);
  if (not model_params.empty()) {
    body.field("options", RawJson::of_raw(model_params));
  }
  body.end_object();

  const HttpResult http = post_json("/api/generate", body.str());
  if (not http.ok) {
    result.error = http.error;
    return result;
  }

  bool found_response = false;
  result.error = with_parsed_object(http.body, [&](simdjson::ondemand::object& obj) {
    std::string_view response;
    if (obj["response"].get_string().get(response)) return;
    result.content = std::string(response);
    found_response = true;
  });

  if (result.error.empty()) {
    if (found_response) {
      result.ok = true;
    } else {
      result.error = "failed to parse ollama response: missing response";
    }
  }

  return result;
}

EmbedResult OllamaClient::embed(std::string_view model,
                                 const std::vector<std::string>& input,
                                 const EmbedOptions& options,
                                 const std::optional<GenerateOptions::ModelParams>& default_params) const {
  EmbedResult result;

  JsonWriter body;
  body.begin_object();
  body.field("model", model);
  body.field("input", input);
  if (options.truncate) body.field("truncate", *options.truncate);
  if (options.dimensions) body.field("dimensions", *options.dimensions);
  if (not options.keep_alive.empty()) body.field("keep_alive", options.keep_alive);

  const GenerateOptions::ModelParams params =
      merge_model_params(options.model_params, default_params);
  const std::string model_params = model_params_to_json(params);
  if (not model_params.empty()) {
    body.field("options", RawJson::of_raw(model_params));
  }
  body.end_object();

  const HttpResult http = post_json("/api/embed", body.str());
  if (not http.ok) {
    result.error = http.error;
    return result;
  }

  result.error = with_parsed_object(http.body, [&](simdjson::ondemand::object& obj) {
    // Read in the order ollama emits these, so the On-Demand cursor only
    // moves forward.
    result.model = string_field(obj, "model");

    simdjson::ondemand::array rows;
    if (not obj["embeddings"].get_array().get(rows)) {
      for (auto row : rows) {
        simdjson::ondemand::array values;
        if (row.get_array().get(values)) continue;
        std::vector<double> embedding;
        for (auto element : values) {
          double v = 0.0;
          if (element.get_double().get(v)) continue;
          embedding.push_back(v);
        }
        result.embeddings.push_back(std::move(embedding));
      }
    }

    result.total_duration = int_field(obj, "total_duration");
    result.load_duration = int_field(obj, "load_duration");
    result.prompt_eval_count = int_field(obj, "prompt_eval_count");
  });

  if (result.error.empty()) result.ok = true;
  return result;
}

ShowResult OllamaClient::show(std::string_view model, bool verbose) const {
  ShowResult result;

  JsonWriter body;
  body.begin_object();
  body.field("model", model);
  if (verbose) body.field("verbose", true);
  body.end_object();

  const HttpResult http = post_json("/api/show", body.str());
  if (not http.ok) {
    result.error = http.error;
    return result;
  }

  result.error = with_parsed_object(http.body, [&](simdjson::ondemand::object& obj) {
    result.license = string_field(obj, "license");
    result.modified_at = string_field(obj, "modified_at");
    result.prompt_template = string_field(obj, "template");
    result.parameters = string_field(obj, "parameters");
    result.model_params = parse_model_params(result.parameters);

    simdjson::ondemand::object details;
    if (not obj["details"].get_object().get(details)) {
      result.details.parent_model = string_field(details, "parent_model");
      result.details.format = string_field(details, "format");
      result.details.family = string_field(details, "family");
      result.details.families = string_array_field(details, "families");
      result.details.parameter_size = string_field(details, "parameter_size");
      result.details.quantization_level =
          string_field(details, "quantization_level");
    }

    result.model_info = RawJson::of_raw(raw_field(obj, "model_info"));
    result.capabilities = string_array_field(obj, "capabilities");
  });

  if (result.error.empty()) result.ok = true;
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
