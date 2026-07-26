#include <core/session_store.hpp>

#include <cstdint>
#include <cstdio>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

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

  nlohmann::json body;
  body["session_id"] = id;
  body["interactions"] = nlohmann::json::array();
  for (const auto& message : interactions) {
    body["interactions"].push_back(
        {{"role", message.role}, {"content", message.content}});
  }

  const std::string path = session_file_path(id);
  std::ofstream out(path, std::ios::trunc);
  if (not out) {
    result.error = "failed to open session file for writing: " + path;
    return result;
  }
  out << body.dump(2);
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

  try {
    const auto parsed = nlohmann::json::parse(buffer.str());
    result.session.session_id = parsed.value("session_id", "");
    if (parsed.contains("interactions")) {
      for (const auto& item : parsed.at("interactions")) {
        ChatMessage message;
        message.role = item.value("role", "");
        message.content = item.value("content", "");
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
