#include <core/mcp/trust.h>

#include <fstream>
#include <sstream>

namespace mcp {

namespace {

std::optional<util::JsonValue> read_json(const std::string& path, bool& exists) {
  std::ifstream in(path, std::ios::binary);
  exists = static_cast<bool>(in);
  if (not exists) return std::nullopt;
  std::stringstream text;
  text << in.rdbuf();
  if (text.str().find_first_not_of(" \t\r\n") == std::string::npos) {
    return util::JsonValue::object();
  }
  std::optional<util::JsonValue> parsed = util::JsonValue::parse(text.str());
  if (not parsed or not parsed->is_object()) return std::nullopt;
  return parsed;
}

std::string era_key(const ServerConfig& server) {
  return std::string(scope_name(server.scope)) + ":" + server.name + ":" +
         config_fingerprint(server).substr(0, 16);
}

}  // namespace

bool needs_approval(const ServerConfig& server) {
  return server.scope != Scope::User;
}

// ─────────────────────────────── TrustStore ─────────────────────────────────

TrustStore::TrustStore(std::string path)
    : mPath(std::move(path)), mData(util::JsonValue::object()) {}

bool TrustStore::load(std::string& error) {
  std::lock_guard<std::mutex> lock(mMutex);
  bool exists = false;
  std::optional<util::JsonValue> parsed = read_json(mPath, exists);
  if (not exists) {
    mData = util::JsonValue::object();
    return true;
  }
  if (not parsed) {
    // Treat it as empty — nothing is approved — rather than guess.
    error = mPath + " is not valid JSON; no MCP server approvals were read";
    mData = util::JsonValue::object();
    return false;
  }
  mData = std::move(*parsed);
  return true;
}

bool TrustStore::save(std::string& error) const {
  std::string text;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    text = mData.dump_pretty() + "\n";
  }
  return write_file_atomically(mPath, text, /*private_file=*/true, error);
}

bool TrustStore::approved(const std::string& workspace,
                          const ServerConfig& server) const {
  std::lock_guard<std::mutex> lock(mMutex);
  const std::string& stored =
      mData.get("workspaces").get(workspace).get(server.name).as_string();
  return not stored.empty() and stored == config_fingerprint(server);
}

void TrustStore::approve(const std::string& workspace, const ServerConfig& server) {
  std::lock_guard<std::mutex> lock(mMutex);
  mData["workspaces"][workspace].set(server.name, config_fingerprint(server));
}

void TrustStore::revoke(const std::string& workspace, const std::string& name) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (util::JsonValue* entries = mData["workspaces"].find(workspace)) {
    entries->erase(name);
  }
}

// ──────────────────────────────── McpState ──────────────────────────────────

McpState::McpState(std::string path)
    : mPath(std::move(path)), mData(util::JsonValue::object()) {}

void McpState::load() {
  std::lock_guard<std::mutex> lock(mMutex);
  bool exists = false;
  std::optional<util::JsonValue> parsed = read_json(mPath, exists);
  mData = parsed ? std::move(*parsed) : util::JsonValue::object();
}

bool McpState::save(std::string& error) const {
  if (mPath.empty()) return true;
  std::string text;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    text = mData.dump_pretty() + "\n";
  }
  return write_file_atomically(mPath, text, /*private_file=*/false, error);
}

Era McpState::era(const ServerConfig& server) const {
  std::lock_guard<std::mutex> lock(mMutex);
  const std::string& stored = mData.get("eras").get(era_key(server)).as_string();
  if (stored == "modern") return Era::Modern;
  if (stored == "legacy") return Era::Legacy;
  return Era::Unknown;
}

void McpState::set_era(const ServerConfig& server, Era era) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (era == Era::Unknown) {
    mData["eras"].erase(era_key(server));
  } else {
    mData["eras"].set(era_key(server), era_name(era));
  }
}

std::optional<bool> McpState::enabled(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mMutex);
  const util::JsonValue& value = mData.get("enabled").get(name);
  if (not value.is_bool()) return std::nullopt;
  return value.as_bool();
}

void McpState::set_enabled(const std::string& name, bool enabled) {
  std::lock_guard<std::mutex> lock(mMutex);
  mData["enabled"].set(name, enabled);
}

}  // namespace mcp
