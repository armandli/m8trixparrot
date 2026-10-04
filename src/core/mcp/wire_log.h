#ifndef M8_MCP_WIRE_LOG_H
#define M8_MCP_WIRE_LOG_H

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace mcp {

// M8_MCP_DEBUG=1: every message to and from a server, appended to
// <logs>/<server>.wire.log for working out what a misbehaving server said.
// Values under keys that look like secrets (tokens, codes, passwords, keys,
// cookies) are replaced before anything is written; headers are not logged.
class WireLog {
public:
  // Null unless M8_MCP_DEBUG is set (and the file opens).
  static std::shared_ptr<WireLog> open_if_enabled(const std::string& path);
  ~WireLog();

  WireLog(const WireLog&) = delete;
  WireLog& operator=(const WireLog&) = delete;

  // '>' for what m8 sent, '<' for what it received.
  void write(char direction, std::string_view message);

private:
  explicit WireLog(std::FILE* file) : mFile(file) {}

  std::mutex mMutex;
  std::FILE* mFile = nullptr;
};

// `text` (JSON) with every value under a secret-looking key replaced by
// "[redacted]"; text that is not JSON comes back as a length only.
std::string redact_for_log(std::string_view text);

}  // namespace mcp

#endif  // M8_MCP_WIRE_LOG_H
