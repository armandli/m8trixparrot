// OAuth for remote MCP servers: the pure rules (challenges, discovery URL
// orders, metadata validation, the RFC 9207 table, token responses, scope
// choice), the credential store, and whole logins against a fake world — an
// MCP server that wants a Bearer token, its protected resource metadata, and
// an authorization server with registration and a token endpoint that checks
// PKCE — with a scripted browser that says yes.

#include <sys/stat.h>

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <core/mcp/credential_store.h>
#include <core/mcp/json_rpc.h>
#include <core/mcp/oauth.h>
#include <core/mcp/registry.h>
#include <core/util/base64.h>
#include <core/util/json_value.h>
#include <core/util/sha256.h>
#include <core/util/url.h>

#include <http_test_server.h>

namespace mcp {
namespace {

using namespace std::chrono_literals;
using m8test::HttpReply;
using m8test::HttpRequest;
using m8test::HttpTestServer;
using util::JsonValue;

JsonValue json(std::string_view text) { return JsonValue::parse(text).value_or(JsonValue()); }

// ─────────────────────────────── the rules ──────────────────────────────────

TEST(McpOAuthRulesTest, ChallengesAreParsedWithQuotesCommasAndCase) {
  const std::string header =
      R"(Basic realm="old", Bearer realm="mcp, the server", error="insufficient_scope", )"
      R"(scope="files:read files:write", resource_metadata="https://x.example/.well-known/oauth-protected-resource", )"
      R"(ERROR_description="needs \"write\"")";
  const std::vector<AuthChallenge> challenges = parse_www_authenticate(header);
  ASSERT_EQ(challenges.size(), 2u);
  EXPECT_EQ(challenges[0].scheme, "basic");
  EXPECT_EQ(challenges[1].scheme, "bearer");
  EXPECT_EQ(challenges[1].params.at("realm"), "mcp, the server");
  EXPECT_EQ(challenges[1].params.at("error_description"), "needs \"write\"");
  EXPECT_EQ(bearer_param(header, "scope"), "files:read files:write");
  EXPECT_EQ(bearer_param(header, "resource_metadata"),
            "https://x.example/.well-known/oauth-protected-resource");
  EXPECT_TRUE(insufficient_scope(header));
  EXPECT_FALSE(insufficient_scope(R"(Bearer error="invalid_token")"));
  EXPECT_EQ(bearer_param("Bearer", "scope"), "");
  EXPECT_EQ(bearer_param("", "scope"), "");
}

TEST(McpOAuthRulesTest, TheResourceIsTheServerUrlInItsCanonicalForm) {
  EXPECT_EQ(canonical_resource("HTTPS://Mcp.Example.COM:443/mcp/"), "https://mcp.example.com/mcp");
  EXPECT_EQ(canonical_resource("https://mcp.example.com/"), "https://mcp.example.com");
  EXPECT_EQ(canonical_resource("https://mcp.example.com:8443/a#frag"), "https://mcp.example.com:8443/a");
}

TEST(McpOAuthRulesTest, DiscoveryTriesTheSpecsUrlsInOrder) {
  EXPECT_EQ(resource_metadata_urls("https://example.com/public/mcp", ""),
            (std::vector<std::string>{
                "https://example.com/.well-known/oauth-protected-resource/public/mcp",
                "https://example.com/.well-known/oauth-protected-resource"}));
  EXPECT_EQ(resource_metadata_urls("https://example.com/", "https://example.com/prm").front(),
            "https://example.com/prm");
  EXPECT_EQ(authorization_server_metadata_urls("https://auth.example.com/tenant1"),
            (std::vector<std::string>{
                "https://auth.example.com/.well-known/oauth-authorization-server/tenant1",
                "https://auth.example.com/.well-known/openid-configuration/tenant1",
                "https://auth.example.com/tenant1/.well-known/openid-configuration"}));
  EXPECT_EQ(authorization_server_metadata_urls("https://auth.example.com"),
            (std::vector<std::string>{
                "https://auth.example.com/.well-known/oauth-authorization-server",
                "https://auth.example.com/.well-known/openid-configuration"}));
}

TEST(McpOAuthRulesTest, ResourceMetadataMustBeAboutThisServer) {
  std::string error;
  const auto prm = [](const std::string& resource) {
    return json(R"({"resource":")" + resource +
                R"(","authorization_servers":["https://auth.example"],"scopes_supported":["a"]})");
  };
  EXPECT_TRUE(parse_resource_metadata(prm("https://mcp.example/mcp"), "https://mcp.example/mcp", error));
  EXPECT_TRUE(parse_resource_metadata(prm("https://mcp.example"), "https://mcp.example/mcp", error));
  EXPECT_FALSE(parse_resource_metadata(prm("https://evil.example/mcp"), "https://mcp.example/mcp", error));
  EXPECT_FALSE(parse_resource_metadata(prm("https://mcp.example/mc"), "https://mcp.example/mcp", error));
  EXPECT_FALSE(parse_resource_metadata(json(R"({"resource":"https://mcp.example"})"),
                                       "https://mcp.example", error));
}

TEST(McpOAuthRulesTest, AuthServerMetadataMustNameItselfAndOfferS256) {
  const std::string good = R"({"issuer":"https://auth.example","authorization_endpoint":"https://auth.example/a",
    "token_endpoint":"https://auth.example/t","code_challenge_methods_supported":["S256"]})";
  std::string error;
  EXPECT_TRUE(parse_auth_server_metadata(json(good), "https://auth.example", error)) << error;
  // A document claiming to be another issuer is somebody else's metadata.
  EXPECT_FALSE(parse_auth_server_metadata(json(good), "https://attacker.example", error));
  EXPECT_NE(error.find("issuer"), std::string::npos);
  EXPECT_FALSE(parse_auth_server_metadata(
      json(R"({"issuer":"https://auth.example","authorization_endpoint":"https://auth.example/a",
              "token_endpoint":"https://auth.example/t"})"),
      "https://auth.example", error));
  EXPECT_NE(error.find("S256"), std::string::npos);
  EXPECT_FALSE(parse_auth_server_metadata(
      json(R"({"issuer":"https://auth.example","authorization_endpoint":"http://auth.example/a",
              "token_endpoint":"https://auth.example/t","code_challenge_methods_supported":["S256"]})"),
      "https://auth.example", error));
  // Plain http is fine where it never leaves the machine.
  EXPECT_TRUE(parse_auth_server_metadata(
      json(R"({"issuer":"http://127.0.0.1:9","authorization_endpoint":"http://127.0.0.1:9/a",
              "token_endpoint":"http://127.0.0.1:9/t","code_challenge_methods_supported":["plain","S256"]})"),
      "http://127.0.0.1:9", error));
}

// The spec's table for the authorization response's iss (RFC 9207).
TEST(McpOAuthRulesTest, TheIssuerTableIsFollowedExactly) {
  const std::string expected = "https://auth.example";
  EXPECT_EQ(check_issuer(true, expected, expected), "");
  EXPECT_NE(check_issuer(true, std::string("https://evil.example"), expected), "");
  EXPECT_NE(check_issuer(true, std::nullopt, expected), "");
  EXPECT_EQ(check_issuer(false, expected, expected), "");
  EXPECT_NE(check_issuer(false, std::string("https://evil.example"), expected), "");
  EXPECT_EQ(check_issuer(false, std::nullopt, expected), "");
  // No normalization: a trailing slash is a different issuer.
  EXPECT_NE(check_issuer(true, std::string("https://auth.example/"), expected), "");
}

TEST(McpOAuthRulesTest, TokenResponsesAreBearerOnly) {
  std::string error;
  const auto tokens = parse_token_response(
      json(R"({"access_token":"a","token_type":"bearer","expires_in":60,"refresh_token":"r","scope":"x"})"),
      1000, error);
  ASSERT_TRUE(tokens);
  EXPECT_EQ(tokens->expires_at, 1060);
  EXPECT_EQ(tokens->refresh_token, "r");
  EXPECT_FALSE(parse_token_response(json(R"({"access_token":"a","token_type":"DPoP"})"), 0, error));
  EXPECT_FALSE(parse_token_response(json(R"({"token_type":"Bearer"})"), 0, error));
}

TEST(McpOAuthRulesTest, ScopesFollowTheChallengeAndGrowOnAStepUp) {
  const std::vector<std::string> offers_refresh = {"files:read", "offline_access"};
  EXPECT_EQ(choose_scope("files:read", "", {"other"}, offers_refresh), "files:read offline_access");
  EXPECT_EQ(choose_scope("", "", {"files:read", "files:list"}, {}), "files:read files:list");
  EXPECT_EQ(choose_scope("files:write", "files:read offline_access", {}, offers_refresh),
            "files:read offline_access files:write");
  // No scope at all stays no scope: never "offline_access" alone.
  EXPECT_EQ(choose_scope("", "", {}, offers_refresh), "");
}

// ─────────────────────────── the credential store ───────────────────────────

struct McpCredentialStoreTest : ::testing::Test {
  std::filesystem::path dir;
  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("m8-mcp-creds-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  void TearDown() override { std::filesystem::remove_all(dir); }
};

TEST_F(McpCredentialStoreTest, TokensAreKeptPrivateAndPerServer) {
  CredentialStore store((dir / "creds.json").string());
  StoredToken token;
  token.server_url = "https://a.example/mcp";
  token.issuer = "https://auth.example";
  token.access_token = "secret";
  token.refresh_token = "r";
  token.expires_at = 42;
  token.token_endpoint = "https://auth.example/token";
  std::string error;
  ASSERT_TRUE(store.put_token(token, error)) << error;

  struct stat info {};
  ASSERT_EQ(::stat((dir / "creds.json").c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600);

  const auto read = store.token("https://a.example/mcp");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->access_token, "secret");
  EXPECT_EQ(read->token_endpoint, "https://auth.example/token");
  EXPECT_FALSE(store.token("https://b.example/mcp"));

  EXPECT_TRUE(store.forget_token("https://a.example/mcp", error));
  EXPECT_FALSE(store.token("https://a.example/mcp"));
  EXPECT_FALSE(store.forget_token("https://a.example/mcp", error));
}

// Writers serialize on the lock file: nobody's entry is lost to a race.
TEST_F(McpCredentialStoreTest, ConcurrentWritersLoseNothing) {
  const std::string path = (dir / "creds.json").string();
  std::vector<std::thread> writers;
  for (int t = 0; t < 8; ++t) {
    writers.emplace_back([&, t] {
      CredentialStore store(path);
      for (int i = 0; i < 10; ++i) {
        StoredToken token;
        token.server_url = "https://s" + std::to_string(t) + "-" + std::to_string(i) + ".example";
        token.access_token = "x";
        std::string error;
        store.put_token(token, error);
      }
    });
  }
  for (std::thread& writer : writers) writer.join();
  CredentialStore store(path);
  int found = 0;
  for (int t = 0; t < 8; ++t) {
    for (int i = 0; i < 10; ++i) {
      if (store.token("https://s" + std::to_string(t) + "-" + std::to_string(i) + ".example")) ++found;
    }
  }
  EXPECT_EQ(found, 80);
}

// ───────────────────────────── a fake world ─────────────────────────────────

std::map<std::string, std::string> query_of(const std::string& url) {
  std::map<std::string, std::string> out;
  const size_t question = url.find('?');
  if (question == std::string::npos) return out;
  for (const auto& [key, value] : util::form_decode(std::string_view(url).substr(question + 1))) {
    out[key] = value;
  }
  return out;
}

long http_get(const std::string& url) {
  CURL* curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   +[](char*, size_t size, size_t count, void*) -> size_t { return size * count; });
  long status = 0;
  if (curl_easy_perform(curl) == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  return status;
}

// One host playing the MCP server (/mcp), its resource metadata, and an
// authorization server at /auth.
struct FakeWorld {
  // Knobs.
  bool iss_supported = true;
  bool send_iss = true;
  std::string iss_override;
  bool s256 = true;
  bool cimd = false;
  bool dcr = true;
  int expires_in = 3600;

  std::mutex mutex;
  std::set<std::string> valid_tokens;
  std::map<std::string, std::string> token_scopes;  // access token -> granted scope
  std::string current_refresh;
  std::string pending_challenge;  // the code_challenge of the authorization in flight
  std::string pending_scope;
  std::map<std::string, std::string> last_authorize;  // what the browser was asked
  std::vector<std::map<std::string, std::string>> token_requests;
  std::vector<std::string> registrations;
  int issued = 0;
  std::vector<std::thread> browsers;

  HttpTestServer http{[this](const HttpRequest& request) { return handle(request); }};

  ~FakeWorld() {
    for (std::thread& browser : browsers) browser.join();
  }

  std::string url(const std::string& path = "") const { return http.url(path); }
  std::string server_url() const { return url("/mcp"); }
  std::string issuer() const { return url("/auth"); }

  // The browser: takes the authorization URL, approves, and is sent back to
  // m8's loopback listener with a code — asynchronously, as a browser is.
  void browse(const std::string& authorization_url) {
    const std::map<std::string, std::string> params = query_of(authorization_url);
    {
      std::lock_guard<std::mutex> lock(mutex);
      last_authorize = params;
      pending_challenge = params.count("code_challenge") ? params.at("code_challenge") : "";
      pending_scope = params.count("scope") ? params.at("scope") : "";
    }
    std::string back = params.at("redirect_uri") + "?code=the-code&state=" +
                       util::percent_encode(params.at("state"));
    if (send_iss) back += "&iss=" + util::percent_encode(iss_override.empty() ? issuer() : iss_override);
    browsers.emplace_back([back] { http_get(back); });
  }

  HttpReply json_reply(int status, const JsonValue& body) const {
    HttpReply reply;
    reply.status = status;
    reply.headers = {{"Content-Type", "application/json"}};
    reply.body = body.dump();
    return reply;
  }

  HttpReply handle(const HttpRequest& request) {
    std::lock_guard<std::mutex> lock(mutex);
    if (request.method == "GET" and request.path == "/.well-known/oauth-protected-resource/mcp") {
      JsonValue prm = JsonValue::object();
      prm.set("resource", server_url());
      JsonValue servers = JsonValue::array();
      servers.push_back(issuer());
      prm.set("authorization_servers", servers);
      return json_reply(200, prm);
    }
    if (request.method == "GET" and request.path == "/.well-known/oauth-authorization-server/auth") {
      JsonValue as = JsonValue::object();
      as.set("issuer", issuer());
      as.set("authorization_endpoint", issuer() + "/authorize");
      as.set("token_endpoint", issuer() + "/token");
      if (dcr) as.set("registration_endpoint", issuer() + "/register");
      JsonValue methods = JsonValue::array();
      if (s256) methods.push_back("S256");
      as.set("code_challenge_methods_supported", methods);
      as.set("scopes_supported", json(R"(["files:read","files:write","offline_access"])"));
      as.set("authorization_response_iss_parameter_supported", iss_supported);
      if (cimd) as.set("client_id_metadata_document_supported", true);
      return json_reply(200, as);
    }
    if (request.method == "POST" and request.path == "/auth/register") {
      registrations.push_back(request.body);
      return json_reply(201, json(R"({"client_id":"client-1"})"));
    }
    if (request.method == "POST" and request.path == "/auth/token") {
      std::map<std::string, std::string> form;
      for (const auto& [key, value] : util::form_decode(request.body)) form[key] = value;
      token_requests.push_back(form);
      const std::string grant = form["grant_type"];
      if (grant == "authorization_code") {
        // PKCE: the verifier must hash to the challenge the browser carried.
        const std::string expected = util::base64_encode(util::sha256_bytes(form["code_verifier"]), true, false);
        if (form["code"] != "the-code" or expected != pending_challenge or
            form["resource"] != server_url()) {
          return json_reply(400, json(R"({"error":"invalid_grant"})"));
        }
      } else if (grant == "refresh_token") {
        if (form["refresh_token"] != current_refresh) {
          return json_reply(400, json(R"({"error":"invalid_grant","error_description":"spent"})"));
        }
      } else {
        return json_reply(400, json(R"({"error":"unsupported_grant_type"})"));
      }
      ++issued;
      const std::string access = "access-" + std::to_string(issued);
      current_refresh = "refresh-" + std::to_string(issued);  // rotated every time
      valid_tokens.insert(access);
      token_scopes[access] = pending_scope;
      JsonValue body = JsonValue::object();
      body.set("access_token", access);
      body.set("token_type", "Bearer");
      body.set("expires_in", expires_in);
      body.set("refresh_token", current_refresh);
      body.set("scope", pending_scope);
      return json_reply(200, body);
    }
    if (request.path == "/mcp") return mcp(request);
    HttpReply missing;
    missing.status = 404;
    return missing;
  }

  HttpReply unauthorized(int status, const std::string& extra) const {
    HttpReply reply;
    reply.status = status;
    reply.headers = {{"WWW-Authenticate",
                      "Bearer resource_metadata=\"" + url("/.well-known/oauth-protected-resource/mcp") +
                          "\", scope=\"files:read\"" + extra}};
    return reply;
  }

  HttpReply mcp(const HttpRequest& request) {
    const std::string authorization = request.header("Authorization");
    const std::string token =
        authorization.rfind("Bearer ", 0) == 0 ? authorization.substr(7) : std::string();
    if (not valid_tokens.count(token)) return unauthorized(401, "");
    const RpcMessage message = parse_message(request.body);
    JsonValue result = JsonValue::object();
    result.set("resultType", "complete");
    if (message.method == "server/discover") {
      result = json(R"({"resultType":"complete","supportedVersions":["2026-07-28"],
        "capabilities":{"tools":{}},"_meta":{"io.modelcontextprotocol/serverInfo":{"name":"files","version":"1"}}})");
    } else if (message.method == "tools/list") {
      result.set("tools", json(R"([{"name":"read","inputSchema":{"type":"object"}},
                                   {"name":"write","inputSchema":{"type":"object"}}])"));
    } else if (message.method == "tools/call") {
      const std::string tool = message.params.get("name").as_string();
      if (tool == "write" and token_scopes[token].find("files:write") == std::string::npos) {
        return unauthorized(403, ", error=\"insufficient_scope\" scope=\"files:write\"");
      }
      JsonValue block = JsonValue::object();
      block.set("type", "text");
      block.set("text", tool + " ok with " + token);
      result["content"].push_back(block);
    }
    HttpReply reply = json_reply(200, JsonValue());
    reply.body = make_result(message.id, result);
    return reply;
  }
};

struct McpOAuthFlowTest : ::testing::Test {
  std::filesystem::path dir;
  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("m8-mcp-oauth-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  void TearDown() override { std::filesystem::remove_all(dir); }

  std::string credentials() const { return (dir / "credentials.json").string(); }

  RegistryOptions options() const {
    RegistryOptions o;
    o.workspace = dir.string();
    o.credentials_path = credentials();
    o.startup_timeout = 5s;
    o.call_timeout = 5s;
    return o;
  }

  static ServerConfig remote(const std::string& url, const std::string& extra = "") {
    std::vector<std::string> warnings;
    return parse_config(R"({"mcpServers":{"files":{"type":"http","url":")" + url + "\"" + extra + "}}}",
                        Scope::User, "test", warnings)
        .front();
  }

  static ServerState state_of(Registry& registry) {
    const CatalogServer* entry = registry.wait_until_settled(5s)->server("files");
    return entry == nullptr ? ServerState::Failed : entry->state;
  }
};

TEST_F(McpOAuthFlowTest, AServerThatWantsALoginGetsOneAndThenWorks) {
  FakeWorld world;
  Registry registry(options());
  registry.start({remote(world.server_url())});
  EXPECT_EQ(state_of(registry), ServerState::NeedsAuth);

  std::string error;
  ASSERT_TRUE(registry.login("files", [&](const std::string& url) { world.browse(url); }, nullptr, error))
      << error;
  EXPECT_EQ(state_of(registry), ServerState::Connected);

  // What the browser was asked for.
  {
    std::lock_guard<std::mutex> lock(world.mutex);
    const auto& asked = world.last_authorize;
    EXPECT_EQ(asked.at("response_type"), "code");
    EXPECT_EQ(asked.at("client_id"), "client-1");
    EXPECT_EQ(asked.at("code_challenge_method"), "S256");
    EXPECT_EQ(asked.at("resource"), world.server_url());
    EXPECT_EQ(asked.at("scope"), "files:read offline_access");
    EXPECT_EQ(asked.at("redirect_uri").rfind("http://127.0.0.1:", 0), 0u);
    EXPECT_GE(asked.at("state").size(), 16u);
    // Registered as a native public client.
    ASSERT_EQ(world.registrations.size(), 1u);
    const JsonValue registration = json(world.registrations[0]);
    EXPECT_EQ(registration.get("application_type").as_string(), "native");
    EXPECT_EQ(registration.get("token_endpoint_auth_method").as_string(), "none");
    EXPECT_EQ(registration.get("redirect_uris").items()[0].as_string(), asked.at("redirect_uri"));
    // The token request carried the verifier and the resource.
    ASSERT_EQ(world.token_requests.size(), 1u);
    EXPECT_EQ(world.token_requests[0].at("resource"), world.server_url());
    EXPECT_FALSE(world.token_requests[0].at("code_verifier").empty());
  }

  const tools::ToolResult result = registry.call_tool("mcp__files__read", "{}", CallContext{});
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.output, "read ok with access-1");

  // Kept, privately, for next time: another registry needs no login.
  struct stat info {};
  ASSERT_EQ(::stat(credentials().c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600);
  registry.shutdown();
  Registry again(options());
  again.start({remote(world.server_url())});
  EXPECT_EQ(state_of(again), ServerState::Connected);
  again.shutdown();
}

TEST_F(McpOAuthFlowTest, ExpiringTokensAreRefreshedAndTheRotationKept) {
  FakeWorld world;
  world.expires_in = 30;  // inside the refresh margin: due at once
  Registry registry(options());
  registry.start({remote(world.server_url())});
  std::string error;
  ASSERT_TRUE(registry.login("files", [&](const std::string& url) { world.browse(url); }, nullptr, error))
      << error;

  const tools::ToolResult result = registry.call_tool("mcp__files__read", "{}", CallContext{});
  ASSERT_TRUE(result.ok) << result.error;
  {
    std::lock_guard<std::mutex> lock(world.mutex);
    ASSERT_GE(world.token_requests.size(), 2u);
    EXPECT_EQ(world.token_requests[1].at("grant_type"), "refresh_token");
    EXPECT_EQ(world.token_requests[1].at("refresh_token"), "refresh-1");
    EXPECT_EQ(world.token_requests[1].at("resource"), world.server_url());
  }
  // The rotated refresh token is what the store holds now.
  CredentialStore store(credentials());
  const auto stored = store.token(world.server_url());
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->refresh_token, world.current_refresh);
  registry.shutdown();
}

TEST_F(McpOAuthFlowTest, ARevokedTokenIsRenewedAndTheRequestSentAgain) {
  FakeWorld world;
  Registry registry(options());
  registry.start({remote(world.server_url())});
  std::string error;
  ASSERT_TRUE(registry.login("files", [&](const std::string& url) { world.browse(url); }, nullptr, error));
  {
    std::lock_guard<std::mutex> lock(world.mutex);
    world.valid_tokens.clear();  // the server forgot every token
  }
  const tools::ToolResult result = registry.call_tool("mcp__files__read", "{}", CallContext{});
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.output, "read ok with access-2");
  registry.shutdown();
}

// RFC 9207: a response naming another issuer is not acted on — not even the
// code it carries is sent anywhere.
TEST_F(McpOAuthFlowTest, AResponseFromTheWrongIssuerIsRefused) {
  FakeWorld world;
  world.iss_override = "https://attacker.example";
  Registry registry(options());
  registry.start({remote(world.server_url())});
  std::string error;
  EXPECT_FALSE(registry.login("files", [&](const std::string& url) { world.browse(url); }, nullptr, error));
  EXPECT_NE(error.find("attacker.example"), std::string::npos) << error;
  std::lock_guard<std::mutex> lock(world.mutex);
  EXPECT_TRUE(world.token_requests.empty());
}

TEST_F(McpOAuthFlowTest, AMissingIssWhenPromisedIsRefused) {
  FakeWorld world;
  world.send_iss = false;
  Registry registry(options());
  registry.start({remote(world.server_url())});
  std::string error;
  EXPECT_FALSE(registry.login("files", [&](const std::string& url) { world.browse(url); }, nullptr, error));
  std::lock_guard<std::mutex> lock(world.mutex);
  EXPECT_TRUE(world.token_requests.empty());
}

TEST_F(McpOAuthFlowTest, AnAuthorizationServerWithoutS256IsRefused) {
  FakeWorld world;
  world.s256 = false;
  Registry registry(options());
  registry.start({remote(world.server_url())});
  std::string error;
  bool browsed = false;
  EXPECT_FALSE(registry.login("files", [&](const std::string&) { browsed = true; }, nullptr, error));
  EXPECT_FALSE(browsed);
  EXPECT_NE(error.find("S256"), std::string::npos) << error;
}

TEST_F(McpOAuthFlowTest, AConfiguredClientOrAMetadataDocumentNeedsNoRegistration) {
  {
    FakeWorld world;
    Registry registry(options());
    registry.start({remote(world.server_url(), R"(,"oauth":{"clientId":"mine"})")});
    std::string error;
    ASSERT_TRUE(registry.login("files", [&](const std::string& url) { world.browse(url); }, nullptr, error))
        << error;
    std::lock_guard<std::mutex> lock(world.mutex);
    EXPECT_EQ(world.last_authorize.at("client_id"), "mine");
    EXPECT_TRUE(world.registrations.empty());
  }
  {
    FakeWorld world;
    world.cimd = true;
    RegistryOptions with_document = options();
    with_document.client_metadata_url = "https://m8.example/client.json";
    std::filesystem::remove(credentials());
    Registry registry(with_document);
    registry.start({remote(world.server_url())});
    std::string error;
    ASSERT_TRUE(registry.login("files", [&](const std::string& url) { world.browse(url); }, nullptr, error))
        << error;
    std::lock_guard<std::mutex> lock(world.mutex);
    EXPECT_EQ(world.last_authorize.at("client_id"), "https://m8.example/client.json");
    EXPECT_TRUE(world.registrations.empty());
  }
}

// A 403 insufficient_scope mid-session: the app is asked, logs in with what
// was granted plus what the call needs, and the call goes once more.
TEST_F(McpOAuthFlowTest, AStepUpAsksAndRetriesWithTheUnionOfScopes) {
  FakeWorld world;
  Registry registry(options());
  int asked = 0;
  registry.set_login_prompt([&](const std::string& server, const std::string& reason) {
    ++asked;
    EXPECT_EQ(server, "files");
    EXPECT_NE(reason.find("permissions"), std::string::npos) << reason;
    std::string error;
    return registry.login(server, [&](const std::string& url) { world.browse(url); }, nullptr, error);
  });
  registry.start({remote(world.server_url())});
  // Connected (and refused) first, so the login knows the scope it asks for.
  ASSERT_EQ(state_of(registry), ServerState::NeedsAuth);
  std::string error;
  ASSERT_TRUE(registry.login("files", [&](const std::string& url) { world.browse(url); }, nullptr, error));

  const tools::ToolResult result = registry.call_tool("mcp__files__write", "{}", CallContext{});
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(asked, 1);
  std::lock_guard<std::mutex> lock(world.mutex);
  EXPECT_EQ(world.last_authorize.at("scope"), "files:read offline_access files:write");
}

TEST_F(McpOAuthFlowTest, WithoutAnAnswerTheLoginCanBeCancelled) {
  FakeWorld world;
  Registry registry(options());
  registry.start({remote(world.server_url())});
  std::atomic<bool> cancel{false};
  std::string error;
  std::thread canceller([&] {
    std::this_thread::sleep_for(300ms);
    cancel.store(true);
  });
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(registry.login("files", [](const std::string&) {}, &cancel, error));
  canceller.join();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
  EXPECT_NE(error.find("cancelled"), std::string::npos) << error;
}

TEST_F(McpOAuthFlowTest, LogoutForgetsTheToken) {
  FakeWorld world;
  Registry registry(options());
  registry.start({remote(world.server_url())});
  std::string error;
  ASSERT_TRUE(registry.login("files", [&](const std::string& url) { world.browse(url); }, nullptr, error));
  ASSERT_TRUE(registry.logout("files", error)) << error;
  EXPECT_FALSE(CredentialStore(credentials()).token(world.server_url()));
  EXPECT_EQ(state_of(registry), ServerState::NeedsAuth);
  registry.shutdown();
}

// A fixed Authorization header in the config means OAuth stays out of it.
TEST_F(McpOAuthFlowTest, AFixedAuthorizationHeaderTurnsOAuthOff) {
  FakeWorld world;
  Registry registry(options());
  registry.start({remote(world.server_url(), R"(,"headers":{"Authorization":"Bearer fixed"})")});
  std::string error;
  EXPECT_FALSE(registry.login("files", [](const std::string&) {}, nullptr, error));
  EXPECT_NE(error.find("fixed Authorization"), std::string::npos) << error;
}

}  // namespace
}  // namespace mcp
