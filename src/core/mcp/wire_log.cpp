#include <core/mcp/wire_log.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include <core/util/json_value.h>

namespace mcp {

namespace {

bool secret_key(std::string_view key) {
  std::string lowered(key);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char* word : {"token", "secret", "password", "passwd", "authorization", "api_key",
                           "apikey", "cookie", "credential", "code_verifier", "private"}) {
    if (lowered.find(word) != std::string::npos) return true;
  }
  return lowered == "code";  // an OAuth authorization code
}

void redact(util::JsonValue& value) {
  if (value.is_object()) {
    for (util::JsonValue::Member& member : value.members()) {
      if (secret_key(member.key) and not member.value.is_null()) {
        member.value = util::JsonValue("[redacted]");
      } else {
        redact(member.value);
      }
    }
  } else if (value.is_array()) {
    for (util::JsonValue& item : value.items()) redact(item);
  }
}

}  // namespace

std::string redact_for_log(std::string_view text) {
  std::optional<util::JsonValue> parsed = util::JsonValue::parse(text);
  if (not parsed) return "(" + std::to_string(text.size()) + " bytes, not JSON)";
  redact(*parsed);
  return parsed->dump();
}

std::shared_ptr<WireLog> WireLog::open_if_enabled(const std::string& path) {
  const char* flag = std::getenv("M8_MCP_DEBUG");
  if (flag == nullptr or *flag == '\0' or std::string(flag) == "0" or path.empty()) {
    return nullptr;
  }
  std::error_code ec;
  const std::filesystem::path target(path);
  if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
  // Private, like the credentials it is careful not to contain.
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) return nullptr;
  std::FILE* file = ::fdopen(fd, "a");
  if (file == nullptr) {
    ::close(fd);
    return nullptr;
  }
  return std::shared_ptr<WireLog>(new WireLog(file));
}

WireLog::~WireLog() {
  if (mFile != nullptr) std::fclose(mFile);
}

void WireLog::write(char direction, std::string_view message) {
  const std::string line = redact_for_log(message);
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm local{};
  ::localtime_r(&seconds, &local);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
  std::lock_guard<std::mutex> lock(mMutex);
  std::fprintf(mFile, "%s.%03d %c %s\n", stamp, static_cast<int>(millis), direction, line.c_str());
  std::fflush(mFile);
}

}  // namespace mcp
