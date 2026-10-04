// MCP tool search: the catalog naming, the BM25 ranking, `select:` resolution,
// and the tool_search tool's effect on an agent's loaded set.

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/mcp/catalog.h>
#include <core/mcp/tool_search.h>
#include <core/tools/tools.h>

namespace mcp {
namespace {

using util::JsonValue;

JsonValue json(const std::string& text) {
  std::optional<JsonValue> value = JsonValue::parse(text);
  EXPECT_TRUE(value.has_value()) << text;
  return value.value_or(JsonValue());
}

ToolInfo tool(const std::string& name, const std::string& description,
              const std::string& schema = R"({"type":"object"})") {
  ToolInfo info;
  info.name = name;
  info.description = description;
  info.input_schema = json(schema);
  return info;
}

std::shared_ptr<const Catalog> catalog_of(
    const std::vector<std::pair<std::string, ToolInfo>>& tools) {
  Catalog catalog;
  std::vector<std::string> servers;
  for (const auto& [server, info] : tools) {
    if (std::find(servers.begin(), servers.end(), server) == servers.end()) {
      servers.push_back(server);
      CatalogServer entry;
      entry.name = server;
      entry.state = ServerState::Connected;
      catalog.servers.push_back(entry);
    }
    CatalogTool built;
    std::string problem;
    EXPECT_TRUE(make_catalog_tool(server, info, false, false, built, problem))
        << problem;
    catalog.tools.push_back(built);
  }
  return finalize_catalog(std::move(catalog));
}

std::shared_ptr<const Catalog> sample() {
  return catalog_of({
      {"github", tool("create_issue", "Create a new issue in a GitHub repository",
                      R"({"type":"object","properties":{"owner":{"type":"string"},"repo":{"type":"string"},"title":{"type":"string","description":"Issue title"}},"required":["owner","repo","title"]})")},
      {"github", tool("list_issues", "List issues in a GitHub repository with filters")},
      {"github", tool("get_pull_request", "Get details of a specific pull request")},
      {"github", tool("search_code", "Search for code across GitHub repositories")},
      {"slack", tool("send_message", "Send a message to a Slack channel",
                     R"({"type":"object","properties":{"channel":{"type":"string"},"text":{"type":"string"}}})")},
      {"slack", tool("list_channels", "List public channels in the workspace")},
      {"fs", tool("read_file", "Read the complete contents of a file from disk")},
      {"fs", tool("write_file", "Create a new file or overwrite an existing one")},
  });
}

std::vector<std::string> top(const Catalog& catalog, const std::string& query,
                             size_t n = 3) {
  const std::vector<SearchHit> hits =
      catalog.index->search(parse_query(query), catalog.tools);
  std::vector<std::string> names;
  for (size_t i = 0; i < hits.size() and i < n; ++i) {
    names.push_back(catalog.tools[hits[i].tool].exposed);
  }
  return names;
}

// ──────────────────────────────── naming ────────────────────────────────────

TEST(McpCatalogTest, ExposedNamesAreIdentifiers) {
  EXPECT_EQ("mcp__github__create_issue", exposed_tool_name("github", "create_issue"));
  EXPECT_EQ("mcp__my_srv__admin_tools_list", exposed_tool_name("my-srv", "admin.tools.list"));
  const std::string long_name = exposed_tool_name("server", std::string(100, 'x'));
  EXPECT_EQ(64u, long_name.size());
  // Stable: the same input always gives the same name.
  EXPECT_EQ(long_name, exposed_tool_name("server", std::string(100, 'x')));
}

TEST(McpCatalogTest, CollidingNamesGetStableSuffixes) {
  const auto catalog = catalog_of({{"s", tool("a-b", "first")}, {"s", tool("a_b", "second")}});
  ASSERT_EQ(2u, catalog->tools.size());
  EXPECT_NE(catalog->tools[0].exposed, catalog->tools[1].exposed);
  EXPECT_NE(nullptr, catalog->find(catalog->tools[1].exposed));
}

TEST(McpCatalogTest, ServerNamesAreValidated) {
  EXPECT_EQ("", server_name_problem("github"));
  EXPECT_EQ("", server_name_problem("my-server_2"));
  EXPECT_NE("", server_name_problem(""));
  EXPECT_NE("", server_name_problem("has space"));
  EXPECT_NE("", server_name_problem("a__b"));
  EXPECT_NE("", server_name_problem(std::string(41, 'a')));
}

TEST(McpCatalogTest, CountsToolsAndTokensPerServer) {
  const auto catalog = sample();
  ASSERT_NE(nullptr, catalog->server("github"));
  EXPECT_EQ(4u, catalog->server("github")->tool_count);
  EXPECT_GT(catalog->total_tokens, 0);
  EXPECT_GT(catalog->server("slack")->schema_tokens, 0);
}

// ──────────────────────────────── search ────────────────────────────────────

TEST(McpSearchTermsTest, SplitsIdentifiersAndFoldsPlurals) {
  EXPECT_EQ((std::vector<std::string>{"get", "http", "response"}),
            search_terms("getHTTPResponse"));
  EXPECT_EQ((std::vector<std::string>{"list", "issue"}), search_terms("list_issues"));
  EXPECT_EQ((std::vector<std::string>{"repository"}), search_terms("the repositories"));
  // Letter/digit boundaries split; a lone letter is dropped, a digit kept.
  EXPECT_EQ((std::vector<std::string>{"2", "export"}), search_terms("v2-export"));
}

TEST(McpSearchTest, RanksTheObviousToolFirst) {
  const auto catalog = sample();
  EXPECT_EQ("mcp__github__create_issue", top(*catalog, "create github issue").front());
  EXPECT_EQ("mcp__slack__send_message", top(*catalog, "send a slack message").front());
  EXPECT_EQ("mcp__fs__read_file", top(*catalog, "read file contents").front());
  EXPECT_EQ("mcp__github__get_pull_request", top(*catalog, "pull request").front());
}

TEST(McpSearchTest, AnExactNameWins) {
  const auto catalog = sample();
  EXPECT_EQ("mcp__github__list_issues", top(*catalog, "list_issues").front());
  EXPECT_EQ("mcp__github__list_issues", top(*catalog, "listissues").front());
}

TEST(McpSearchTest, ServerAndRequiredFilters) {
  const auto catalog = sample();
  const std::vector<std::string> slack = top(*catalog, "server:slack list", 5);
  ASSERT_FALSE(slack.empty());
  for (const std::string& name : slack) {
    EXPECT_EQ(0u, name.rfind("mcp__slack__", 0)) << name;
  }
  const std::vector<std::string> required = top(*catalog, "+channel list", 5);
  ASSERT_EQ(1u, required.size());
  EXPECT_EQ("mcp__slack__list_channels", required.front());
  // server: alone lists the server.
  EXPECT_EQ(2u, top(*catalog, "server:fs", 10).size());
}

TEST(McpSearchTest, PrefixesMatchWhenWholeWordsDoNot) {
  const auto catalog = sample();
  EXPECT_EQ("mcp__slack__list_channels", top(*catalog, "chan").front());
}

// ────────────────────────────── select: names ───────────────────────────────

TEST(McpResolveTest, AcceptsTheSpellingsModelsUse) {
  const auto catalog = sample();
  for (const char* name : {"mcp__github__list_issues", "github__list_issues",
                           "github.list_issues", "github/list_issues",
                           "github:list_issues", "list_issues", "LIST_ISSUES",
                           "listissues"}) {
    const NameResolution resolution = resolve_tool_name(*catalog, name);
    ASSERT_NE(nullptr, resolution.tool) << name;
    EXPECT_EQ("mcp__github__list_issues", resolution.tool->exposed) << name;
  }
  EXPECT_EQ(nullptr, resolve_tool_name(*catalog, "delete_everything").tool);
}

TEST(McpResolveTest, AmbiguousBareNamesListCandidates) {
  const auto catalog = catalog_of({{"a", tool("search", "x")}, {"b", tool("search", "y")}});
  const NameResolution resolution = resolve_tool_name(*catalog, "search");
  EXPECT_EQ(nullptr, resolution.tool);
  EXPECT_EQ(2u, resolution.candidates.size());
}

// ─────────────────────────────── the tool ───────────────────────────────────

tools::ToolResult run(const Catalog& catalog, LoadedTools& loaded,
                      const std::string& query, int64_t limit = 0) {
  tools::ToolArgs args;
  args["query"] = query;
  if (limit > 0) args["limit"] = limit;
  return ToolSearchTool{catalog, loaded}.execute(args);
}

TEST(McpToolSearchToolTest, KeywordSearchLoadsTheStrongMatchesOnly) {
  const auto catalog = sample();
  LoadedTools loaded;
  const tools::ToolResult result = run(*catalog, loaded, "create github issue");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(loaded.contains("mcp__github__create_issue"));
  EXPECT_LE(loaded.size(), 5u);
  // The signature tells the model how to call it.
  EXPECT_NE(std::string::npos,
            result.output.find("mcp__github__create_issue(owner: string"))
      << result.output;
}

TEST(McpToolSearchToolTest, SelectLoadsExactlyWhatWasNamed) {
  const auto catalog = sample();
  LoadedTools loaded;
  const tools::ToolResult result =
      run(*catalog, loaded, "select:slack:send_message, fs__read_file");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ((std::vector<std::string>{"mcp__slack__send_message", "mcp__fs__read_file"}),
            loaded.names());

  const tools::ToolResult again = run(*catalog, loaded, "select:read_file");
  EXPECT_TRUE(again.ok);
  EXPECT_NE(std::string::npos, again.output.find("Already loaded")) << again.output;
  EXPECT_EQ(2u, loaded.size());
}

TEST(McpToolSearchToolTest, UnknownNamesSuggestButNeverGuess) {
  const auto catalog = sample();
  LoadedTools loaded;
  const tools::ToolResult result = run(*catalog, loaded, "select:create_issues_now");
  EXPECT_FALSE(result.ok);
  EXPECT_NE(std::string::npos, result.error.find("closest")) << result.error;
  EXPECT_EQ(0u, loaded.size());
}

TEST(McpToolSearchToolTest, NoMatchSaysWhatExists) {
  const auto catalog = sample();
  LoadedTools loaded;
  const tools::ToolResult result = run(*catalog, loaded, "kubernetes pods");
  EXPECT_TRUE(result.ok);
  EXPECT_NE(std::string::npos, result.output.find("github (4 tools)")) << result.output;
  EXPECT_EQ(0u, loaded.size());
}

TEST(McpToolSearchToolTest, AnEmptyQueryIsAnError) {
  const auto catalog = sample();
  LoadedTools loaded;
  EXPECT_FALSE(run(*catalog, loaded, "  ").ok);
}

// ───────────────────────────── loaded set ───────────────────────────────────

TEST(McpLoadedToolsTest, EvictsLeastRecentlyUsedAndVanishedTools) {
  const auto catalog = sample();
  LoadedTools loaded;
  loaded.add("mcp__github__create_issue");
  loaded.add("mcp__github__list_issues");
  loaded.add("mcp__slack__send_message");
  loaded.add("mcp__gone__tool");
  loaded.touch("mcp__github__create_issue");  // now the most recent

  const std::vector<std::string> dropped = loaded.evict(2, 0, *catalog);
  EXPECT_EQ(2u, dropped.size());
  EXPECT_FALSE(loaded.contains("mcp__gone__tool"));
  EXPECT_FALSE(loaded.contains("mcp__github__list_issues"));
  EXPECT_TRUE(loaded.contains("mcp__github__create_issue"));
}

}  // namespace
}  // namespace mcp
