// What m8 shows about MCP: pure formatters over hand-built catalog snapshots,
// so none of this needs a server or a terminal.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <m8_mcp_view.h>

namespace m8 {
namespace {

using mcp::ServerState;

constexpr auto npos = std::string::npos;

mcp::CatalogServer server(const std::string& name, ServerState state,
                          size_t tool_count = 0) {
  mcp::CatalogServer entry;
  entry.name = name;
  entry.scope = "user";
  entry.transport = "stdio";
  entry.detail = "run-" + name + " --stdio";
  entry.state = state;
  entry.tool_count = tool_count;
  entry.protocol_version = "2026-07-28";
  return entry;
}

mcp::CatalogTool tool(const std::string& server, const std::string& name,
                      int64_t tokens, bool always_load = false) {
  mcp::CatalogTool entry;
  entry.server = server;
  entry.name = name;
  entry.exposed = "mcp__" + server + "__" + name;
  entry.description = name + " does a thing";
  entry.tokens = tokens;
  entry.always_load = always_load;
  return entry;
}

mcp::PromptInfo prompt(const std::string& name,
                       std::vector<mcp::PromptArgument> arguments) {
  mcp::PromptInfo info;
  info.name = name;
  info.description = name + " prompt";
  info.arguments = std::move(arguments);
  return info;
}

mcp::Catalog prompt_catalog() {
  mcp::Catalog catalog;
  mcp::CatalogServer fake = server("fake", ServerState::Connected);
  fake.prompts = {prompt("review", {{"code", "", true}}),
                  prompt("greet", {{"name", "", true}, {"lang", "", false},
                                   {"style", "", false}})};
  catalog.servers = {fake};
  mcp::CatalogServer down = server("down", ServerState::Failed);
  down.prompts = {prompt("gone", {})};
  catalog.servers.push_back(down);
  return catalog;
}

TEST(McpViewTest, HumanTokens) {
  EXPECT_EQ(human_tokens(640), "640");
  EXPECT_EQ(human_tokens(1600), "1.6k");
  EXPECT_EQ(human_tokens(23500), "23.5k");
}

// `m8 mcp get` is safe to paste into an issue: a reference shows which
// variable feeds a header; a literal token never shows.
TEST(McpViewTest, MaskSecretShowsReferencesAndNeverALiteral) {
  EXPECT_EQ(mask_secret("${GITHUB_TOKEN}"), "${GITHUB_TOKEN}");
  EXPECT_EQ(mask_secret("Bearer ${GITHUB_TOKEN}"), "Bearer ${GITHUB_TOKEN}");
  EXPECT_EQ(mask_secret("${USER}:${PASS}"), "${USER}:${PASS}");
  EXPECT_EQ(mask_secret(""), "");
  EXPECT_EQ(mask_secret("hunter2").find("hunter2"), npos);
  EXPECT_EQ(mask_secret("Bearer sk-live-123").find("sk-live"), npos);
  EXPECT_EQ(mask_secret("sk-live${X}").find("sk-live"), npos);
  EXPECT_EQ(mask_secret("pre${UNCLOSED").find("UNCLOSED"), npos);
}

TEST(McpViewTest, HeaderCountsConnectedServersOfTheEnabledOnes) {
  mcp::Catalog catalog;
  EXPECT_EQ(mcp_header(catalog), "");

  catalog.servers = {server("a", ServerState::Connected),
                     server("b", ServerState::Connected),
                     server("c", ServerState::Connected),
                     server("d", ServerState::Failed),
                     server("e", ServerState::Disabled)};
  EXPECT_EQ(mcp_header(catalog), "mcp 3/4");

  // Something only the user can resolve gets a mark.
  catalog.servers.push_back(server("f", ServerState::NeedsApproval));
  EXPECT_EQ(mcp_header(catalog), "mcp 3/5 !");
}

TEST(McpViewTest, ContextLineSplitsLoadedFromDeferred) {
  mcp::Catalog catalog;
  catalog.servers = {server("gh", ServerState::Connected, 3)};
  catalog.tools = {tool("gh", "a", 1000), tool("gh", "b", 600),
                   tool("gh", "c", 400, /*always_load=*/true)};

  EXPECT_EQ(mcp_context_line(catalog, /*deferred=*/true, {"mcp__gh__a"}),
            "mcp: 3 tools \xc2\xb7 loaded 2 (~1.4k) \xc2\xb7 deferred 1 (~600 saved "
            "per call)");
  EXPECT_EQ(mcp_context_line(catalog, /*deferred=*/false, {}),
            "mcp: 3 tools \xc2\xb7 all loaded, 3 (~2.0k)");

  catalog.tools.clear();
  EXPECT_EQ(mcp_context_line(catalog, true, {}), "");
}

TEST(McpViewTest, ToolsTextMarksEachToolLoadedDeferredOrAlways) {
  mcp::Catalog catalog;
  catalog.servers = {server("gh", ServerState::Connected, 3),
                     server("off", ServerState::Failed)};
  catalog.tools = {tool("gh", "a", 1000), tool("gh", "b", 600),
                   tool("gh", "c", 400, /*always_load=*/true)};

  const std::string text = mcp_tools_text(catalog, "", /*deferred=*/true, {"mcp__gh__a"});
  EXPECT_NE(text.find("loaded   mcp__gh__a"), npos) << text;
  EXPECT_NE(text.find("deferred mcp__gh__b"), npos) << text;
  EXPECT_NE(text.find("always   mcp__gh__c"), npos) << text;

  // Without deferral every tool is in the tools array.
  const std::string all = mcp_tools_text(catalog, "gh", /*deferred=*/false, {});
  EXPECT_NE(all.find("loaded   mcp__gh__b"), npos) << all;

  EXPECT_NE(mcp_tools_text(catalog, "off", true, {}).find("No connected"), npos);

  const std::string plain = mcp_tools_text(catalog, "gh", false, {}, /*status=*/false);
  EXPECT_NE(plain.find("\n  mcp__gh__a"), npos) << plain;
  EXPECT_EQ(plain.find("loaded"), npos) << plain;
}

TEST(McpViewTest, StatusShowsEachServerAndWhatToDoAboutIt) {
  mcp::Catalog catalog;
  catalog.servers = {server("gh", ServerState::Connected, 1),
                     server("crashy", ServerState::Failed),
                     server("foo", ServerState::NeedsApproval),
                     server("cloud", ServerState::NeedsAuth)};
  catalog.servers[1].error = "exited with status 1";
  catalog.servers[1].stderr_tail = "ModuleNotFoundError: mcp";
  catalog.servers[2].scope = "shared";
  catalog.tools = {tool("gh", "a", 1000)};

  const std::string text = mcp_status_text(catalog, mcp::ToolSearchSettings{},
                                           128000, /*deferred=*/false, {});
  EXPECT_NE(text.find("2026-07-28"), npos) << text;
  EXPECT_NE(text.find("exited with status 1"), npos) << text;
  EXPECT_NE(text.find("ModuleNotFoundError"), npos) << text;
  EXPECT_NE(text.find("run-foo --stdio"), npos) << text;
  EXPECT_NE(text.find("/mcp approve foo"), npos) << text;
  EXPECT_NE(text.find("/mcp login cloud"), npos) << text;

  EXPECT_NE(mcp_status_text(mcp::Catalog{}, mcp::ToolSearchSettings{}, 0, false, {})
                .find("m8 mcp add"),
            npos);
}

TEST(McpViewTest, PendingApprovalNamesEachServerAndItsCommand) {
  mcp::Catalog catalog;
  catalog.servers = {server("ok", ServerState::Connected),
                     server("foo", ServerState::NeedsApproval)};
  const std::string text = pending_approval_text(catalog);
  EXPECT_NE(text.find("foo"), npos) << text;
  EXPECT_NE(text.find("run-foo --stdio"), npos) << text;
  EXPECT_NE(text.find("/mcp approve"), npos) << text;
  EXPECT_EQ(text.find("run-ok"), npos) << text;

  catalog.servers.pop_back();
  EXPECT_EQ(pending_approval_text(catalog), "");
}

// One declared argument takes the whole rest of the line, so a prompt about
// code needs no quoting.
TEST(McpViewTest, APromptWithOneArgumentTakesTheWholeLine) {
  const auto command =
      parse_prompt_command("/mcp__fake__review fix the bug in main.cpp", prompt_catalog());
  ASSERT_TRUE(command.has_value());
  EXPECT_EQ(command->error, "");
  EXPECT_EQ(command->server, "fake");
  EXPECT_EQ(command->prompt, "review");
  EXPECT_EQ(command->arguments.get("code").string_or(""), "fix the bug in main.cpp");
}

TEST(McpViewTest, PromptArgumentsArePositionalOrNamed) {
  const mcp::Catalog catalog = prompt_catalog();

  auto command = parse_prompt_command("/mcp__fake__greet Ada lang=fr", catalog);
  ASSERT_TRUE(command.has_value());
  EXPECT_EQ(command->error, "");
  EXPECT_EQ(command->arguments.get("name").string_or(""), "Ada");
  EXPECT_EQ(command->arguments.get("lang").string_or(""), "fr");
  EXPECT_FALSE(command->arguments.contains("style"));

  command = parse_prompt_command("/mcp__fake__greet \"Ada Lovelace\" en formal", catalog);
  ASSERT_TRUE(command.has_value());
  EXPECT_EQ(command->arguments.get("name").string_or(""), "Ada Lovelace");
  EXPECT_EQ(command->arguments.get("lang").string_or(""), "en");
  EXPECT_EQ(command->arguments.get("style").string_or(""), "formal");

  command = parse_prompt_command("/mcp__fake__greet lang=fr", catalog);
  ASSERT_TRUE(command.has_value());
  EXPECT_NE(command->error.find("missing <name>"), npos) << command->error;
  EXPECT_NE(command->error.find("/mcp__fake__greet <name> [lang] [style]"), npos)
      << command->error;

  command = parse_prompt_command("/mcp__fake__greet a b c d", catalog);
  ASSERT_TRUE(command.has_value());
  EXPECT_NE(command->error.find("too many"), npos) << command->error;
}

TEST(McpViewTest, OnlyAConnectedServersPromptIsACommand) {
  const mcp::Catalog catalog = prompt_catalog();
  EXPECT_FALSE(parse_prompt_command("/mcp__nope__review x", catalog).has_value());
  EXPECT_FALSE(parse_prompt_command("/mcp__down__gone", catalog).has_value());
  EXPECT_FALSE(parse_prompt_command("/help", catalog).has_value());
}

TEST(McpViewTest, PromptCommandNamesAreIdentifierSafe) {
  EXPECT_EQ(prompt_command_name("my-srv", "code.review"), "mcp__my_srv__code_review");

  const std::string help = prompt_help_text(prompt_catalog());
  EXPECT_NE(help.find("/mcp__fake__review <code>"), npos) << help;
  EXPECT_NE(help.find("greet prompt"), npos) << help;
  EXPECT_EQ(help.find("gone"), npos) << help;
  EXPECT_EQ(prompt_help_text(mcp::Catalog{}), "");
}

TEST(McpViewTest, ServerConfigTextMasksEverySecret) {
  mcp::ServerConfig http;
  http.name = "gh";
  http.type = "http";
  http.url = "https://api.example.com/mcp";
  http.headers = {{"Authorization", "Bearer sk-live-123"}, {"X-Ref", "${REF}"}};
  std::string text = server_config_text(http);
  EXPECT_NE(text.find("https://api.example.com/mcp"), npos) << text;
  EXPECT_EQ(text.find("sk-live"), npos) << text;
  EXPECT_NE(text.find("${REF}"), npos) << text;

  mcp::ServerConfig stdio;
  stdio.name = "local";
  stdio.type = "stdio";
  stdio.command = "npx";
  stdio.args = {"-y", "some-server"};
  stdio.env = {{"TOKEN", "hunter2"}, {"HOME_REF", "${HOME}"}};
  text = server_config_text(stdio);
  EXPECT_NE(text.find("npx -y some-server"), npos) << text;
  EXPECT_EQ(text.find("hunter2"), npos) << text;
  EXPECT_NE(text.find("${HOME}"), npos) << text;
}

TEST(McpViewTest, ToolDisplayNameSplitsServerFromTool) {
  EXPECT_EQ(tool_display_name("mcp__github__create_issue"),
            "github \xe2\x80\xba create_issue");
  // Server names cannot hold "__"; a tool name can.
  EXPECT_EQ(tool_display_name("mcp__srv__my__tool"), "srv \xe2\x80\xba my__tool");
  EXPECT_EQ(tool_display_name("bash_repl"), "bash_repl");
  EXPECT_EQ(tool_display_name("mcp__"), "mcp__");
  EXPECT_EQ(tool_display_name("mcp__srv__"), "mcp__srv__");
}

}  // namespace
}  // namespace m8
