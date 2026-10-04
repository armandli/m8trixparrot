#ifndef M8_MCP_OAUTH_H
#define M8_MCP_OAUTH_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/mcp/config.h>
#include <core/mcp/credential_store.h>
#include <core/util/json_value.h>

namespace mcp {

// ---------------------------------------------------------------------------
// OAuth for remote MCP servers, as the 2026-07-28 authorization spec has it:
// protected resource metadata (RFC 9728) points at the authorization server,
// whose metadata (RFC 8414 or OIDC discovery) must name itself exactly and
// support S256; the client is pre-configured, a Client ID Metadata Document,
// or dynamically registered (in that order); the browser is sent to the
// authorization endpoint with PKCE, state and the resource indicator
// (RFC 8707); the redirect comes back to a loopback listener, is checked for
// state and issuer (RFC 9207), and the code is exchanged for tokens that are
// kept in the credential store and refreshed as they expire.
// ---------------------------------------------------------------------------

// One challenge of a WWW-Authenticate header: `Bearer realm="x", scope="a b"`.
struct AuthChallenge {
  std::string scheme;                         // lowercase
  std::map<std::string, std::string> params;  // names lowercase, values unquoted
};

std::vector<AuthChallenge> parse_www_authenticate(std::string_view header);
// A parameter of the Bearer challenge, or "".
std::string bearer_param(std::string_view header, const std::string& name);
// A 403 whose challenge says the token lacks scope (step-up).
bool insufficient_scope(std::string_view www_authenticate);

// The URI tokens are requested for (RFC 8707): the server URL with a
// lowercase scheme and host, no fragment, and no trailing slash.
std::string canonical_resource(const std::string& server_url);

// Where to look for the server's protected resource metadata, in order: the
// challenge's resource_metadata, else the well-known URI with the endpoint's
// path inserted, then at the root.
std::vector<std::string> resource_metadata_urls(const std::string& server_url,
                                                const std::string& from_challenge);
// Where to look for an authorization server's metadata, in the spec's order:
// RFC 8414 and OIDC with path insertion, then OIDC with the path appended.
std::vector<std::string> authorization_server_metadata_urls(const std::string& issuer);

struct ResourceMetadata {
  std::string resource;
  std::vector<std::string> authorization_servers;
  std::vector<std::string> scopes_supported;
};
// The resource must be the server URL or an ancestor of it, on the same
// origin (RFC 9728 section 3.3, as the reference SDKs apply it).
std::optional<ResourceMetadata> parse_resource_metadata(const util::JsonValue& document,
                                                        const std::string& server_url,
                                                        std::string& error);

struct AuthServerMetadata {
  std::string issuer;
  std::string authorization_endpoint;
  std::string token_endpoint;
  std::string registration_endpoint;
  std::vector<std::string> scopes_supported;
  std::vector<std::string> token_endpoint_auth_methods;
  bool client_id_metadata_document_supported = false;
  bool iss_parameter_supported = false;
};
// Refused unless its issuer is exactly `expected_issuer`, it lists S256, and
// every endpoint is https (or loopback).
std::optional<AuthServerMetadata> parse_auth_server_metadata(const util::JsonValue& document,
                                                             const std::string& expected_issuer,
                                                             std::string& error);

// RFC 9207 on the redirect: "" when the response may be used, else why not.
std::string check_issuer(bool iss_parameter_supported, const std::optional<std::string>& iss,
                         const std::string& expected_issuer);

struct TokenSet {
  std::string access_token;
  std::string refresh_token;
  std::string scope;
  int64_t expires_at = 0;  // unix seconds; 0: unknown
};
std::optional<TokenSet> parse_token_response(const util::JsonValue& document, int64_t now,
                                             std::string& error);

// The scopes to ask for: the challenge's (joined with what was asked before,
// on a step-up), else the resource's, plus offline_access when the
// authorization server offers it.
std::string choose_scope(const std::string& challenge_scope, const std::string& previous,
                         const std::vector<std::string>& resource_scopes,
                         const std::vector<std::string>& server_scopes);

// http(s) URL rules for OAuth endpoints: https, or http on loopback.
bool secure_endpoint(const std::string& url);

struct OAuthOptions {
  std::string server;      // the configured name, for messages
  std::string server_url;  // the MCP endpoint
  OAuthSettings settings;  // clientId, clientSecret, scopes, callbackPort
  std::string client_metadata_url;  // m8's CIMD document, if one is hosted
  std::string credentials_path;     // ~/.m8/mcp_credentials.json
  std::chrono::milliseconds http_timeout{10000};
  std::chrono::seconds login_timeout{300};
  // Refresh this long before the token says it expires.
  std::chrono::seconds refresh_margin{60};
};

class LoopbackListener;

// A login waiting for the browser to come back.
class PendingLogin {
public:
  ~PendingLogin();
  const std::string& url() const { return mUrl; }
  int port() const;
  // Blocks until the redirect arrives (then exchanges the code and stores the
  // tokens), the login times out, cancel() is called, or `also_cancelled`
  // turns true.
  bool wait(std::string& error, const std::atomic<bool>* also_cancelled = nullptr);
  void cancel();

private:
  friend class OAuthSession;
  PendingLogin() = default;

  class OAuthSession* mSession = nullptr;
  std::unique_ptr<LoopbackListener> mListener;
  std::string mUrl;
  std::string mIssuer;
  bool mIssSupported = false;
  std::string mState;
  std::string mVerifier;
  std::string mRedirectUri;
  std::string mClientId;
  std::string mClientSecret;
  bool mBasicAuth = false;
  std::string mTokenEndpoint;
  std::string mResource;
  std::string mScope;
  std::atomic<bool> mCancelled{false};
};

// One remote server's credentials: the Authorization header for its
// requests, kept fresh, and the interactive login that obtains them. Shared
// by the server's transports (thread-safe).
class OAuthSession {
public:
  explicit OAuthSession(OAuthOptions options);

  // "Bearer <token>" for a request now, refreshed first when it is about to
  // expire; "" without a token (the server's 401 then asks for a login).
  std::string authorization();
  // The server refused the token (401): refresh it, or pick up one another
  // m8 just stored. True when there is a new one to retry with.
  bool renew_after_rejection();
  // A 401 or 403 challenge: kept for the next login (its scope and metadata
  // location), and on a step-up the scopes asked for grow.
  void challenge(const std::string& www_authenticate);

  // Discovery, registration and the authorization URL; the listener is up.
  std::shared_ptr<PendingLogin> begin_login(std::string& error);
  bool logout(std::string& error);
  bool has_token();
  const OAuthOptions& options() const { return mOptions; }

private:
  friend class PendingLogin;
  bool refresh_locked(std::string& error);  // mMutex held
  void reload_locked();                     // mMutex held
  bool finish(PendingLogin& login, const std::string& code, std::string& error);

  OAuthOptions mOptions;
  CredentialStore mStore;
  std::string mKey;  // the server URL tokens are kept under

  std::mutex mMutex;
  std::optional<StoredToken> mToken;
  bool mLoaded = false;
  int64_t mStoreStamp = -1;  // the credential file's mtime when last read
  std::string mChallenge;    // the last WWW-Authenticate
  std::string mStepUpScope;  // scopes a 403 asked for, joined with earlier ones
};

}  // namespace mcp

#endif  // M8_MCP_OAUTH_H
