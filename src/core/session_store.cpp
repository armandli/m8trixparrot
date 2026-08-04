#include <core/session_store.hpp>

#include <cstdint>
#include <cstdio>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <simdjson.h>

#include <core/json_util.hpp>

namespace agent {

namespace {

bool is_valid_session_id(const std::string& id) {
  if (id.empty()) return false;
  if (id == "." or id == "..") return false;
  if (id.find('/') != std::string::npos) return false;
  return true;
}

// RFC 4122 version-4 (random) UUID: 16 random bytes with the version and
// variant bits overwritten, formatted as the canonical 8-4-4-4-12 hex string.
std::string generate_uuid_v4() {
  static thread_local std::mt19937_64 engine(std::random_device{}());
  std::uniform_int_distribution<uint64_t> dist;

  uint64_t hi = dist(engine);
  uint64_t lo = dist(engine);

  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  char buf[37];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                static_cast<unsigned>(hi >> 32),
                static_cast<unsigned>((hi >> 16) & 0xFFFFu),
                static_cast<unsigned>(hi & 0xFFFFu),
                static_cast<unsigned>(lo >> 48),
                static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
  return std::string(buf);
}

}  // namespace

SessionStore::SessionStore(std::string root_dir) : root_dir_(std::move(root_dir)) {}

std::string SessionStore::session_file_path(const std::string& session_id) const {
  return (std::filesystem::path(root_dir_) / (session_id + ".json")).string();
}

SessionStoreResult SessionStore::store(const std::vector<ChatMessage>& interactions,
                                        const std::string& session_id) const {
  SessionStoreResult result;

  std::string id = session_id;
  if (id.empty()) {
    id = generate_uuid_v4();
  } else if (not is_valid_session_id(id)) {
    result.error = "invalid session id: " + id;
    return result;
  }

  std::error_code ec;
  std::filesystem::create_directories(root_dir_, ec);
  if (ec) {
    result.error = "failed to create session root directory: " + ec.message();
    return result;
  }

  JsonWriter body;
  body.begin_object();
  body.field("session_id", id);
  body.key("interactions").begin_array();
  for (const auto& message : interactions) {
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
        body.field("name", call.name);
        body.field("arguments",
                    RawJson::of_raw(call.arguments.empty() ? "{}"
                                                           : call.arguments));
        body.end_object();
      }
      body.end_array();
    }
    body.end_object();
  }
  body.end_array();
  body.end_object();

  // simdjson's builder emits minified JSON; session files stay human-readable
  // by running the result through its FracturedJson formatter.
  simdjson::fractured_json_options format_options;
  format_options.indent_spaces = 2;
  // One field per line, like nlohmann's dump(2) produced: message content is
  // long and arbitrary, so column alignment and multiple records per line
  // both hurt more than they help.
  format_options.enable_table_format = false;
  format_options.enable_compact_multiline = false;
  format_options.max_inline_length = 0;
  format_options.max_inline_complexity = 0;
  const std::string text =
      simdjson::fractured_json_string(body.str(), format_options);

  const std::string path = session_file_path(id);
  std::ofstream out(path, std::ios::trunc);
  if (not out) {
    result.error = "failed to open session file for writing: " + path;
    return result;
  }
  out << text;
  if (not out) {
    result.error = "failed to write session file: " + path;
    return result;
  }

  result.ok = true;
  result.session_id = id;
  return result;
}

SessionResult SessionStore::load(const std::string& session_id) const {
  SessionResult result;
  if (not is_valid_session_id(session_id)) {
    result.error = "invalid session id: " + session_id;
    return result;
  }
  return load_from_path(session_file_path(session_id));
}

SessionResult SessionStore::load_from_path(const std::string& path) const {
  SessionResult result;

  std::ifstream in(path);
  if (not in) {
    result.error = "session file not found: " + path;
    return result;
  }

  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string contents = buffer.str();

  try {
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(contents);
    simdjson::ondemand::document doc = parser.iterate(padded);

    simdjson::ondemand::object root;
    if (auto error = doc.get_object().get(root)) {
      result.error = std::string("failed to parse session file: ") +
                     simdjson::error_message(error);
      return result;
    }

    result.session.session_id = string_field(root, "session_id");

    simdjson::ondemand::array interactions;
    if (not root["interactions"].get_array().get(interactions)) {
      for (auto item : interactions) {
        simdjson::ondemand::object entry;
        if (item.get_object().get(entry)) continue;
        ChatMessage message;
        message.role = string_field(entry, "role");
        message.content = string_field(entry, "content");
        message.tool_name = string_field(entry, "tool_name");

        simdjson::ondemand::array calls;
        if (not entry["tool_calls"].get_array().get(calls)) {
          for (auto call_item : calls) {
            simdjson::ondemand::object call;
            if (call_item.get_object().get(call)) continue;
            ToolCall tool_call;
            tool_call.name = string_field(call, "name");
            tool_call.arguments = raw_field(call, "arguments", "{}");
            if (tool_call.name.empty()) continue;
            message.tool_calls.push_back(std::move(tool_call));
          }
        }

        result.session.interactions.push_back(std::move(message));
      }
    }
    result.ok = true;
  } catch (const std::exception& e) {
    result.error = std::string("failed to parse session file: ") + e.what();
  }

  return result;
}

SessionResult SessionStore::latest() const {
  SessionResult result;

  std::error_code ec;
  if (not std::filesystem::exists(root_dir_, ec) or ec) {
    result.error = "session root directory does not exist: " + root_dir_;
    return result;
  }

  std::filesystem::path latest_path;
  std::filesystem::file_time_type latest_time;
  bool found = false;

  for (const auto& entry : std::filesystem::directory_iterator(root_dir_, ec)) {
    if (not entry.is_regular_file()) continue;
    if (entry.path().extension() != ".json") continue;

    const auto write_time = entry.last_write_time();
    if (not found or write_time > latest_time) {
      found = true;
      latest_time = write_time;
      latest_path = entry.path();
    }
  }

  if (not found) {
    result.error = "no session files found in " + root_dir_;
    return result;
  }

  return load_from_path(latest_path.string());
}

}  // namespace agent
