#include <core/mcp/credential_store.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <core/mcp/config.h>

namespace mcp {

namespace {

using util::JsonValue;

// The lock file beside the store: flock on the store itself would not survive
// the rename that replaces it.
class FileLock {
public:
  FileLock(const std::string& path, int operation) {
    std::error_code ec;
    const std::filesystem::path target(path);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    mFd = ::open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (mFd < 0) {
      mError = "could not open " + path + ".lock: " + std::strerror(errno);
      return;
    }
    while (::flock(mFd, operation) != 0) {
      if (errno == EINTR) continue;
      mError = "could not lock " + path + ": " + std::strerror(errno);
      ::close(mFd);
      mFd = -1;
      return;
    }
  }
  ~FileLock() {
    if (mFd >= 0) {
      ::flock(mFd, LOCK_UN);
      ::close(mFd);
    }
  }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  bool held() const { return mFd >= 0; }
  const std::string& error() const { return mError; }

private:
  int mFd = -1;
  std::string mError;
};

StoredToken token_from(const std::string& server_url, const JsonValue& entry) {
  StoredToken token;
  token.server_url = server_url;
  token.issuer = entry.get("issuer").as_string();
  token.resource = entry.get("resource").as_string();
  token.client_id = entry.get("client_id").as_string();
  token.access_token = entry.get("access_token").as_string();
  token.refresh_token = entry.get("refresh_token").as_string();
  token.scope = entry.get("scope").as_string();
  token.expires_at = entry.get("expires_at").as_int();
  token.token_endpoint = entry.get("token_endpoint").as_string();
  return token;
}

JsonValue token_entry(const StoredToken& token) {
  JsonValue entry = JsonValue::object();
  entry.set("issuer", token.issuer);
  entry.set("resource", token.resource);
  entry.set("client_id", token.client_id);
  entry.set("access_token", token.access_token);
  if (not token.refresh_token.empty()) entry.set("refresh_token", token.refresh_token);
  if (not token.scope.empty()) entry.set("scope", token.scope);
  if (token.expires_at > 0) entry.set("expires_at", token.expires_at);
  if (not token.token_endpoint.empty()) entry.set("token_endpoint", token.token_endpoint);
  return entry;
}

}  // namespace

CredentialStore::CredentialStore(std::string path) : mPath(std::move(path)) {}

JsonValue CredentialStore::read() const {
  std::ifstream in(mPath, std::ios::binary);
  if (not in) return JsonValue::object();
  std::stringstream text;
  text << in.rdbuf();
  std::optional<JsonValue> document = JsonValue::parse(text.str());
  if (not document or not document->is_object()) return JsonValue::object();
  return *document;
}

bool CredentialStore::write(const JsonValue& document, std::string& error) {
  return write_file_atomically(mPath, document.dump_pretty() + "\n", /*private_file=*/true,
                               error);
}

std::optional<StoredToken> CredentialStore::token_unlocked(const std::string& server_url) const {
  const JsonValue document = read();
  const JsonValue* entry = document.get("tokens").find(server_url);
  if (entry == nullptr or not entry->is_object()) return std::nullopt;
  StoredToken token = token_from(server_url, *entry);
  if (token.access_token.empty()) return std::nullopt;
  return token;
}

std::optional<StoredToken> CredentialStore::token(const std::string& server_url) const {
  FileLock lock(mPath, LOCK_SH);
  if (not lock.held()) return std::nullopt;
  return token_unlocked(server_url);
}

std::optional<StoredClient> CredentialStore::client(const std::string& issuer) const {
  FileLock lock(mPath, LOCK_SH);
  if (not lock.held()) return std::nullopt;
  const JsonValue document = read();
  const JsonValue* entry = document.get("clients").find(issuer);
  if (entry == nullptr or not entry->is_object()) return std::nullopt;
  StoredClient client;
  client.issuer = issuer;
  client.client_id = entry->get("client_id").as_string();
  client.client_secret = entry->get("client_secret").as_string();
  client.secret_expires_at = entry->get("client_secret_expires_at").as_int();
  client.redirect_uri = entry->get("redirect_uri").as_string();
  if (client.client_id.empty()) return std::nullopt;
  return client;
}

bool CredentialStore::put_token_unlocked(const StoredToken& token, std::string& error) {
  JsonValue document = read();
  document.set("version", 1);
  document["tokens"].set(token.server_url, token_entry(token));
  return write(document, error);
}

bool CredentialStore::put_token(const StoredToken& token, std::string& error) {
  FileLock lock(mPath, LOCK_EX);
  if (not lock.held()) {
    error = lock.error();
    return false;
  }
  return put_token_unlocked(token, error);
}

bool CredentialStore::put_client(const StoredClient& client, std::string& error) {
  FileLock lock(mPath, LOCK_EX);
  if (not lock.held()) {
    error = lock.error();
    return false;
  }
  JsonValue document = read();
  document.set("version", 1);
  JsonValue entry = JsonValue::object();
  entry.set("client_id", client.client_id);
  if (not client.client_secret.empty()) entry.set("client_secret", client.client_secret);
  if (client.secret_expires_at > 0) entry.set("client_secret_expires_at", client.secret_expires_at);
  if (not client.redirect_uri.empty()) entry.set("redirect_uri", client.redirect_uri);
  document["clients"].set(client.issuer, entry);
  return write(document, error);
}

bool CredentialStore::forget_token(const std::string& server_url, std::string& error) {
  FileLock lock(mPath, LOCK_EX);
  if (not lock.held()) {
    error = lock.error();
    return false;
  }
  JsonValue document = read();
  if (not document["tokens"].erase(server_url)) return false;
  return write(document, error);
}

bool CredentialStore::exclusive(const std::function<void()>& work, std::string& error) {
  FileLock lock(mPath, LOCK_EX);
  if (not lock.held()) {
    error = lock.error();
    return false;
  }
  work();
  return true;
}

}  // namespace mcp
