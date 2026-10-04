#ifndef M8_MCP_CREDENTIAL_STORE_H
#define M8_MCP_CREDENTIAL_STORE_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <core/util/json_value.h>

namespace mcp {

// A client registered with an authorization server (dynamic registration),
// kept per issuer: client ids are only good at the server that issued them.
struct StoredClient {
  std::string issuer;
  std::string client_id;
  std::string client_secret;
  int64_t secret_expires_at = 0;  // unix seconds; 0: never
  std::string redirect_uri;       // the one it was registered with
};

// What a login produced for one MCP server, keyed by the server's URL so a
// token is only ever sent to the server it was obtained for.
struct StoredToken {
  std::string server_url;
  std::string issuer;
  std::string resource;  // what the token was requested for (RFC 8707)
  std::string client_id;
  std::string access_token;
  std::string refresh_token;
  std::string scope;
  int64_t expires_at = 0;      // unix seconds; 0: unknown
  std::string token_endpoint;  // where a refresh goes
};

// ~/.m8/mcp_credentials.json: mode 0600, replaced whole through a temp file
// and a rename, and changed only under an exclusive flock on a lock file
// beside it, so two m8 processes refreshing the same token cannot lose a
// rotated refresh token between them. Every read goes to the file: another
// process may have changed it.
struct CredentialStore {
  explicit CredentialStore(std::string path);

  std::optional<StoredToken> token(const std::string& server_url) const;
  std::optional<StoredClient> client(const std::string& issuer) const;

  bool put_token(const StoredToken& token, std::string& error);
  bool put_client(const StoredClient& client, std::string& error);
  // Forgets the server's token; false (with no error) when there was none.
  bool forget_token(const std::string& server_url, std::string& error);

  // Runs `work` holding the exclusive lock — for a refresh, which must read,
  // call the authorization server and write without another process in
  // between. The other members take the lock themselves; inside `work`, use
  // the *_unlocked forms.
  bool exclusive(const std::function<void()>& work, std::string& error);
  std::optional<StoredToken> token_unlocked(const std::string& server_url) const;
  bool put_token_unlocked(const StoredToken& token, std::string& error);

  const std::string& path() const { return mPath; }

private:
  util::JsonValue read() const;
  bool write(const util::JsonValue& document, std::string& error);

  std::string mPath;
};

}  // namespace mcp

#endif  // M8_MCP_CREDENTIAL_STORE_H
