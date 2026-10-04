// MCP server configuration: the three files and their precedence, variable
// expansion, editing in place, and the approval fingerprint that decides when
// a workspace server must be approved again.

#include <sys/stat.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <core/mcp/config.h>
#include <core/mcp/trust.h>

namespace mcp {
namespace {

using util::JsonValue;

struct McpConfigTest : ::testing::Test {
  std::filesystem::path dir;

  void SetUp() override {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    dir = std::filesystem::temp_directory_path() /
          ("m8-mcp-config-" + std::string(info->name()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  void TearDown() override { std::filesystem::remove_all(dir); }

  std::string write(const std::string& name, const std::string& text) const {
    const std::filesystem::path path = dir / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << text;
    return path.string();
  }

  static std::string read(const std::string& path) {
    std::ifstream in(path);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
  }

  static std::vector<ServerConfig> parse(const std::string& text) {
    std::vector<std::string> warnings;
    return parse_config(text, Scope::Project, "test.json", warnings);
  }
};

TEST_F(McpConfigTest, TypesAreInferredAndAliasesAccepted) {
  const std::vector<ServerConfig> servers = parse(R"({"mcpServers":{
      "fs":{"command":"npx","args":["-y","pkg"],"env":{"PORT":8080,"DEBUG":true}},
      "remote":{"url":"https://example.com/mcp"},
      "aliased":{"type":"streamable-http","url":"https://e.com/mcp","headers":{"Authorization":"Bearer x"}}}})");
  ASSERT_EQ(3u, servers.size());
  EXPECT_EQ("stdio", servers[0].type);
  EXPECT_EQ((std::vector<std::string>{"-y", "pkg"}), servers[0].args);
  ASSERT_EQ(2u, servers[0].env.size());
  EXPECT_EQ("8080", servers[0].env[0].second);
  EXPECT_EQ("true", servers[0].env[1].second);
  EXPECT_EQ("http", servers[1].type);
  EXPECT_EQ("http", servers[2].type);
  EXPECT_EQ("Bearer x", servers[2].headers[0].second);
  for (const ServerConfig& server : servers) EXPECT_EQ("", server.problem) << server.name;
}

TEST_F(McpConfigTest, UnusableEntriesAreKeptWithTheReason) {
  std::vector<std::string> warnings;
  const std::vector<ServerConfig> servers = parse_config(R"({"mcpServers":{
      "old":{"type":"sse","url":"https://e.com/sse"},
      "nothing":{},
      "bad name":{"command":"x"},
      "weird":{"command":"x","protocol":"v2"},
      "ftp":{"type":"http","url":"ftp://e.com/mcp"}}})", Scope::User, "f.json", warnings);
  ASSERT_EQ(5u, servers.size());
  EXPECT_NE(std::string::npos, servers[0].problem.find("Streamable HTTP"));
  EXPECT_NE("", servers[1].problem);
  EXPECT_NE("", servers[2].problem);
  EXPECT_NE("", servers[3].problem);
  EXPECT_NE(std::string::npos, servers[4].problem.find("http://"));
  EXPECT_EQ(5u, warnings.size());

  // A URL that is all one variable is checked once it is expanded.
  warnings.clear();
  const std::vector<ServerConfig> deferred = parse_config(
      R"({"mcpServers":{"v":{"type":"http","url":"${MCP_URL}"},
                        "u":{"url":"HTTPS://E.COM/mcp"}}})", Scope::User, "f.json", warnings);
  ASSERT_EQ(2u, deferred.size());
  EXPECT_EQ("", deferred[0].problem);
  EXPECT_EQ("", deferred[1].problem);
}

TEST_F(McpConfigTest, MalformedFilesWarnInsteadOfFailing) {
  std::vector<std::string> warnings;
  EXPECT_TRUE(parse_config("{nope", Scope::User, "broken.json", warnings).empty());
  ASSERT_EQ(1u, warnings.size());
  EXPECT_NE(std::string::npos, warnings[0].find("broken.json"));
  EXPECT_TRUE(parse_config("", Scope::User, "empty.json", warnings).empty());
  EXPECT_EQ(1u, warnings.size());
}

TEST_F(McpConfigTest, ProjectBeatsSharedBeatsUser) {
  ConfigFiles files;
  files.user = write("home/.m8/mcp.json",
                     R"({"mcpServers":{"a":{"command":"user-a"},"u":{"command":"only-user"}}})");
  files.shared = write("ws/.mcp.json", R"({"mcpServers":{"a":{"command":"shared-a"},"s":{"command":"only-shared"}}})");
  files.project = write("ws/.m8/mcp.json", R"({"mcpServers":{"a":{"command":"project-a"}}})");
  const LoadedConfig loaded = load_config(files);
  ASSERT_EQ(3u, loaded.servers.size());
  EXPECT_EQ("a", loaded.servers[0].name);
  EXPECT_EQ("project-a", loaded.servers[0].command);
  EXPECT_EQ(Scope::Project, loaded.servers[0].scope);
  EXPECT_EQ(Scope::User, loaded.servers[1].scope);
  EXPECT_EQ(Scope::Shared, loaded.servers[2].scope);
}

// With the workspace root at $HOME, the project file is the user file. Read
// twice, every user server would turn into a project server needing approval.
TEST_F(McpConfigTest, AWorkspaceAtHomeReadsTheFileOnceAsUser) {
  ConfigFiles files;
  files.user = write("home/.m8/mcp.json", R"({"mcpServers":{"a":{"command":"x"}}})");
  files.project = (dir / "home/.m8/../.m8/mcp.json").string();
  const LoadedConfig loaded = load_config(files);
  ASSERT_EQ(1u, loaded.servers.size());
  EXPECT_EQ(Scope::User, loaded.servers[0].scope);
}

TEST_F(McpConfigTest, VariablesExpandFromTheEnvironment) {
  ::setenv("M8_TEST_TOKEN", "secret", 1);
  ::unsetenv("M8_TEST_UNSET");
  std::string missing;
  EXPECT_EQ("Bearer secret", expand_variables("Bearer ${M8_TEST_TOKEN}", missing));
  EXPECT_EQ("fallback", expand_variables("${M8_TEST_UNSET:-fallback}", missing));
  EXPECT_EQ("", missing);
  EXPECT_EQ("$NOT_BRACED stays", expand_variables("$NOT_BRACED stays", missing));
  expand_variables("${M8_TEST_UNSET}", missing);
  EXPECT_EQ("M8_TEST_UNSET", missing);

  const std::vector<ServerConfig> servers = parse(
      R"({"mcpServers":{"s":{"url":"https://x/${M8_TEST_TOKEN}","headers":{"Authorization":"Bearer ${M8_TEST_UNSET}"}}}})");
  ServerConfig expanded;
  std::string error;
  EXPECT_FALSE(expand_server(servers[0], expanded, error));
  EXPECT_NE(std::string::npos, error.find("M8_TEST_UNSET"));
  ::unsetenv("M8_TEST_TOKEN");
}

TEST_F(McpConfigTest, FingerprintTracksWhatRunsOnly) {
  const auto print = [](const std::string& entry) {
    return config_fingerprint(parse(R"({"mcpServers":{"s":)" + entry + "}}").front());
  };
  const std::string base = print(R"({"command":"npx","args":["-y","pkg"],"env":{"A":"1","B":"2"}})");
  // Order, and the knobs that do not change what runs, keep an approval.
  EXPECT_EQ(base, print(R"({"env":{"B":"2","A":"1"},"args":["-y","pkg"],"command":"npx"})"));
  EXPECT_EQ(base, print(R"({"command":"npx","args":["-y","pkg"],"env":{"A":"1","B":"2"},"alwaysLoad":true,"disabledTools":["x"],"timeout":5})"));
  // What runs does not.
  EXPECT_NE(base, print(R"({"command":"npx","args":["-y","evil"],"env":{"A":"1","B":"2"}})"));
  EXPECT_NE(base, print(R"({"command":"sh","args":["-y","pkg"],"env":{"A":"1","B":"2"}})"));
  EXPECT_NE(base, print(R"({"command":"npx","args":["-y","pkg"],"env":{"A":"1","B":"3"}})"));
}

TEST_F(McpConfigTest, EditingKeepsEverythingElse) {
  const std::string path = write(".m8/mcp.json", R"({
  "comment": "kept",
  "mcpServers": {"keep": {"command": "x", "customKey": [1, 2]}}
})");
  ASSERT_TRUE(add_server(path, "fs", stdio_entry("npx", {"-y", "pkg"}, {{"K", "V"}})).ok);
  EXPECT_FALSE(add_server(path, "fs", http_entry("https://e/mcp", {})).ok);
  ASSERT_TRUE(add_server(path, "fs", http_entry("https://e/mcp", {}), true).ok);

  const JsonValue written = *JsonValue::parse(read(path));
  EXPECT_EQ("kept", written.get("comment").as_string());
  EXPECT_EQ(2, written.get("mcpServers").get("keep").get("customKey").items()[1].as_int());
  EXPECT_EQ("https://e/mcp", written.get("mcpServers").get("fs").get("url").as_string());

  ASSERT_TRUE(remove_server(path, "fs").ok);
  EXPECT_FALSE(remove_server(path, "fs").ok);
  EXPECT_FALSE(JsonValue::parse(read(path))->get("mcpServers").contains("fs"));

  // It may hold tokens: owner-only.
  struct stat info {};
  ASSERT_EQ(0, ::stat(path.c_str(), &info));
  EXPECT_EQ(0600u, info.st_mode & 0777u);
}

TEST_F(McpConfigTest, AddingToAMissingFileCreatesIt) {
  const std::string path = (dir / "new/.m8/mcp.json").string();
  ASSERT_TRUE(add_server(path, "fs", stdio_entry("npx", {}, {})).ok);
  std::vector<std::string> warnings;
  EXPECT_EQ(1u, parse_config(read(path), Scope::Project, path, warnings).size());
}

TEST_F(McpConfigTest, ApprovalsFollowTheFingerprint) {
  const std::string trust_path = (dir / "trust.json").string();
  ServerConfig server = parse(R"({"mcpServers":{"s":{"command":"npx","args":["a"]}}})").front();
  EXPECT_TRUE(needs_approval(server));

  {
    TrustStore trust(trust_path);
    std::string error;
    ASSERT_TRUE(trust.load(error));
    EXPECT_FALSE(trust.approved("/ws", server));
    trust.approve("/ws", server);
    ASSERT_TRUE(trust.save(error)) << error;
  }
  TrustStore reloaded(trust_path);
  std::string error;
  ASSERT_TRUE(reloaded.load(error));
  EXPECT_TRUE(reloaded.approved("/ws", server));
  EXPECT_FALSE(reloaded.approved("/other-ws", server));

  ServerConfig changed = parse(R"({"mcpServers":{"s":{"command":"npx","args":["b"]}}})").front();
  EXPECT_FALSE(reloaded.approved("/ws", changed));

  reloaded.revoke("/ws", "s");
  EXPECT_FALSE(reloaded.approved("/ws", server));
}

TEST_F(McpConfigTest, StateRemembersErasAndEnableChoices) {
  const std::string path = (dir / "state.json").string();
  const ServerConfig server = parse(R"({"mcpServers":{"s":{"command":"x"}}})").front();
  {
    McpState state(path);
    state.load();
    EXPECT_EQ(Era::Unknown, state.era(server));
    state.set_era(server, Era::Legacy);
    state.set_enabled("s", false);
    std::string error;
    ASSERT_TRUE(state.save(error)) << error;
  }
  McpState reloaded(path);
  reloaded.load();
  EXPECT_EQ(Era::Legacy, reloaded.era(server));
  EXPECT_EQ(false, reloaded.enabled("s").value_or(true));
  EXPECT_FALSE(reloaded.enabled("other").has_value());
}

// Connect threads record their eras while the UI thread records an enable
// choice, all saving at once: every save must succeed, and the file must end
// up with every change, not an older snapshot or two interleaved ones.
TEST_F(McpConfigTest, ConcurrentSavesKeepEveryChange) {
  const std::vector<ServerConfig> servers = parse(R"({"mcpServers":{
      "a":{"command":"x"},"b":{"command":"x"},"c":{"command":"x"},"d":{"command":"x"},
      "e":{"command":"x"},"f":{"command":"x"},"g":{"command":"x"},"h":{"command":"x"}}})");
  ASSERT_EQ(8u, servers.size());
  for (int round = 0; round < 50; ++round) {
    const std::string path = (dir / ("state-" + std::to_string(round) + ".json")).string();
    McpState state(path);
    state.load();
    std::atomic<bool> go{false};
    std::atomic<int> failed{0};
    const auto save = [&state, &failed] {
      std::string error;
      if (not state.save(error)) ++failed;
    };
    std::vector<std::thread> threads;
    for (size_t i = 0; i < servers.size(); ++i) {
      threads.emplace_back([&, i] {
        while (not go.load()) std::this_thread::yield();
        state.set_era(servers[i], Era::Legacy);
        save();
      });
    }
    threads.emplace_back([&] {
      while (not go.load()) std::this_thread::yield();
      state.set_enabled("a", false);
      save();
    });
    go.store(true);
    for (std::thread& thread : threads) thread.join();

    EXPECT_EQ(0, failed.load()) << "round " << round;
    McpState reloaded(path);
    reloaded.load();
    for (const ServerConfig& server : servers) {
      EXPECT_EQ(Era::Legacy, reloaded.era(server)) << server.name << " in round " << round;
    }
    EXPECT_EQ(false, reloaded.enabled("a").value_or(true)) << "round " << round;
    if (HasFailure()) return;
  }
}

}  // namespace
}  // namespace mcp
