// `m8 mcp ...`, parsed through CLI11 the way main() declares it, against a
// temp workspace and a temp home; `list` and `get` start the scripted server.

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <CLI/CLI.hpp>
#include <gtest/gtest.h>

#include <core/mcp/config.h>
#include <core/mcp/trust.h>
#include <core/util/json_value.h>

#include <m8_mcp_cli.h>
#include <m8_paths.h>

namespace m8 {
namespace {

namespace fs = std::filesystem;

constexpr auto npos = std::string::npos;

struct CliRun {
  int status = 0;
  std::string out;
  std::string err;
};

struct McpCliTest : ::testing::Test {
  void SetUp() override {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    mRoot = fs::temp_directory_path() / ("m8-mcp-cli-test-" + std::string(info->name()));
    fs::remove_all(mRoot);
    fs::create_directories(mRoot / "repo/.git");
    fs::create_directories(mRoot / "home/.m8");
    mRoot = fs::canonical(mRoot);
    mPaths = resolve_m8_paths((mRoot / "repo").string(), (mRoot / "home").string());
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(mRoot, ec);
  }

  // Parses `args` with main()'s neighbours declared too — the model positional
  // and the top-level -s,--session — and runs whatever was parsed.
  CliRun run(const std::vector<std::string>& args, bool interactive = false,
          bool agent_shell = false, const std::string& input = "") {
    CLI::App app{"m8"};
    std::string model;
    std::string session;
    app.add_option("model,-m,--model", model);
    app.add_option("-s,--session", session);
    McpCli cli;
    cli.declare(app);
    cli.interactive = interactive;

    std::vector<const char*> argv{"m8"};
    for (const std::string& arg : args) argv.push_back(arg.c_str());
    CliRun result;
    try {
      app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError& error) {
      result.status = -1;
      result.err = error.what();
      return result;
    }
    if (not cli.parsed()) {
      result.status = -2;
      return result;
    }
    std::ostringstream out;
    std::ostringstream err;
    std::istringstream in(input);
    result.status = cli.run(mPaths, agent_shell, out, err, in);
    result.out = out.str();
    result.err = err.str();
    return result;
  }

  static util::JsonValue read_json(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return util::JsonValue::parse(text.str()).value_or(util::JsonValue());
  }

  util::JsonValue entry(const std::string& path, const std::string& name) const {
    return read_json(path).get("mcpServers").get(name);
  }

  bool approved(const std::string& name) const {
    mcp::ConfigFiles files;
    files.user = mPaths.user_mcp_config();
    files.shared = mPaths.shared_mcp_config();
    files.project = mPaths.mcp_config();
    mcp::TrustStore trust(mPaths.mcp_trust());
    std::string error;
    trust.load(error);
    for (const mcp::ServerConfig& server : mcp::load_config(files).servers) {
      if (server.name == name) return trust.approved(workspace_key(mPaths), server);
    }
    return false;
  }

  fs::path mRoot;
  M8Paths mPaths;
};

TEST_F(McpCliTest, AddWritesAStdioServerToTheProjectFile) {
  const CliRun r = run({"mcp", "add", "everything", "--", "npx", "-y",
                     "@modelcontextprotocol/server-everything"});
  ASSERT_EQ(r.status, 0) << r.err;

  const util::JsonValue added = entry(mPaths.mcp_config(), "everything");
  EXPECT_EQ(added.get("command").string_or(""), "npx");
  ASSERT_EQ(added.get("args").size(), 2u);
  EXPECT_EQ(added.get("args").items()[0].string_or(""), "-y");
  EXPECT_EQ(added.get("args").items()[1].string_or(""),
            "@modelcontextprotocol/server-everything");

  // From a script rather than a terminal: added, and waiting for approval.
  EXPECT_NE(r.out.find("m8 mcp approve everything"), npos) << r.out;
  EXPECT_FALSE(approved("everything"));
}

// -e takes exactly one value, so the server's name is never swallowed as a
// second one; the value keeps any '=' of its own.
TEST_F(McpCliTest, RepeatedEnvFlagsLeaveTheNameAlone) {
  const CliRun r = run({"mcp", "add", "-e", "A=1", "-e", "B=x=y", "srv", "--", "cmd", "arg"});
  ASSERT_EQ(r.status, 0) << r.err;
  const util::JsonValue added = entry(mPaths.mcp_config(), "srv");
  EXPECT_EQ(added.get("command").string_or(""), "cmd");
  EXPECT_EQ(added.get("env").get("A").string_or(""), "1");
  EXPECT_EQ(added.get("env").get("B").string_or(""), "x=y");
}

TEST_F(McpCliTest, AUrlMakesAnHttpServer) {
  const CliRun r = run({"mcp", "add", "-s", "user", "gh", "https://example.com/mcp", "-H",
                     "Authorization: Bearer ${GH_TOKEN}"});
  ASSERT_EQ(r.status, 0) << r.err;
  const util::JsonValue added = entry(mPaths.user_mcp_config(), "gh");
  EXPECT_EQ(added.get("type").string_or(""), "http");
  EXPECT_EQ(added.get("url").string_or(""), "https://example.com/mcp");
  EXPECT_EQ(added.get("headers").get("Authorization").string_or(""),
            "Bearer ${GH_TOKEN}");
  // The user's own file needs no approval, and the project file is untouched.
  EXPECT_EQ(r.out.find("approve"), npos) << r.out;
  EXPECT_FALSE(fs::exists(mPaths.mcp_config()));
}

TEST_F(McpCliTest, MismatchedFlagsAreRefused) {
  EXPECT_EQ(run({"mcp", "add", "srv", "--", "cmd", "-H", "A: b"}).status, 0)
      << "after --, -H is the command's own argument";
  EXPECT_EQ(run({"mcp", "add", "-H", "A: b", "srv2", "cmd"}).status, 1);
  EXPECT_EQ(run({"mcp", "add", "-e", "A=1", "web", "https://example.com"}).status, 1);
  EXPECT_EQ(run({"mcp", "add", "-e", "novalue", "srv3", "cmd"}).status, 1);
  EXPECT_EQ(run({"mcp", "add", "-s", "galaxy", "srv4", "cmd"}).status, -1);
}

TEST_F(McpCliTest, SharedScopeWritesTheCrossToolFile) {
  ASSERT_EQ(run({"mcp", "add", "-s", "shared", "srv", "--", "cmd"}).status, 0);
  EXPECT_TRUE(read_json(mPaths.shared_mcp_config()).get("mcpServers").contains("srv"));
  EXPECT_FALSE(approved("srv"));
}

// Someone typed it at a terminal: that is the approval.
TEST_F(McpCliTest, AddingAtATerminalApprovesTheServer) {
  const CliRun r = run({"mcp", "add", "srv", "--", "cmd"}, /*interactive=*/true);
  ASSERT_EQ(r.status, 0) << r.err;
  EXPECT_TRUE(approved("srv"));
}

TEST_F(McpCliTest, AddJsonTakesAnEntryAsWritten) {
  const CliRun r = run({"mcp", "add-json", "srv",
                     R"({"command":"cmd","args":["a"],"alwaysLoad":true,"custom":1})"});
  ASSERT_EQ(r.status, 0) << r.err;
  const util::JsonValue added = entry(mPaths.mcp_config(), "srv");
  EXPECT_EQ(added.get("command").string_or(""), "cmd");
  EXPECT_TRUE(added.get("alwaysLoad").as_bool());
  EXPECT_EQ(added.get("custom").as_int(), 1);
}

TEST_F(McpCliTest, ChangesAreRefusedInAnAgentsShell) {
  const std::vector<std::vector<std::string>> changes = {
      {"mcp", "add", "srv", "--", "cmd"},
      {"mcp", "add-json", "srv", R"({"command":"cmd"})"},
      {"mcp", "remove", "srv"},
      {"mcp", "enable", "srv"},
      {"mcp", "approve", "srv"}};
  for (const std::vector<std::string>& args : changes) {
    const CliRun r = run(args, /*interactive=*/true, /*agent_shell=*/true);
    EXPECT_EQ(r.status, 1) << args[1];
    EXPECT_NE(r.err.find("refused"), npos) << r.err;
  }
  EXPECT_FALSE(fs::exists(mPaths.mcp_config()));

  // Turning a server off stays allowed.
  ASSERT_EQ(run({"mcp", "add", "srv", "--", "cmd"}).status, 0);
  EXPECT_EQ(run({"mcp", "disable", "srv"}, false, /*agent_shell=*/true).status, 0);
}

TEST_F(McpCliTest, AddRefusesWhatCouldNeverRun) {
  CliRun r = run({"mcp", "add-json", "old", R"({"type":"sse","url":"https://x"})"});
  EXPECT_EQ(r.status, 1);
  EXPECT_NE(r.err.find("old"), npos) << r.err;

  r = run({"mcp", "add", "bad__name", "--", "cmd"});
  EXPECT_EQ(r.status, 1);
  EXPECT_NE(r.err.find("bad__name"), npos) << r.err;

  r = run({"mcp", "add-json", "x", "not json"});
  EXPECT_EQ(r.status, 1);
  r = run({"mcp", "add-json", "x", "[1]"});
  EXPECT_EQ(r.status, 1);

  EXPECT_FALSE(fs::exists(mPaths.mcp_config()));
}

TEST_F(McpCliTest, AddRefusesANameTheFileAlreadyHas) {
  ASSERT_EQ(run({"mcp", "add", "srv", "--", "cmd"}).status, 0);
  const CliRun r = run({"mcp", "add", "srv", "--", "other"});
  EXPECT_EQ(r.status, 1);
  EXPECT_NE(r.err.find("already exists"), npos) << r.err;
  EXPECT_EQ(entry(mPaths.mcp_config(), "srv").get("command").string_or(""), "cmd");
}

TEST_F(McpCliTest, RemoveFindsTheFileAndForgetsTheApproval) {
  ASSERT_EQ(run({"mcp", "add", "srv", "--", "cmd"}, /*interactive=*/true).status, 0);
  ASSERT_TRUE(approved("srv"));

  const CliRun r = run({"mcp", "remove", "srv"});
  ASSERT_EQ(r.status, 0) << r.err;
  EXPECT_FALSE(read_json(mPaths.mcp_config()).get("mcpServers").contains("srv"));

  // Added back later, the same entry is a new decision.
  ASSERT_EQ(run({"mcp", "add", "srv", "--", "cmd"}).status, 0);
  EXPECT_FALSE(approved("srv"));

  EXPECT_EQ(run({"mcp", "remove", "nope"}).status, 1);
}

TEST_F(McpCliTest, RemoveAsksWhichFileWhenTwoHoldTheName) {
  ASSERT_EQ(run({"mcp", "add", "srv", "--", "a"}).status, 0);
  ASSERT_EQ(run({"mcp", "add", "-s", "user", "srv", "--", "b"}).status, 0);

  CliRun r = run({"mcp", "remove", "srv"});
  EXPECT_EQ(r.status, 1);
  EXPECT_NE(r.err.find("-s"), npos) << r.err;

  r = run({"mcp", "remove", "-s", "user", "srv"});
  ASSERT_EQ(r.status, 0) << r.err;
  EXPECT_TRUE(read_json(mPaths.mcp_config()).get("mcpServers").contains("srv"));
  EXPECT_FALSE(read_json(mPaths.user_mcp_config()).get("mcpServers").contains("srv"));
}

// Remembered per workspace, never written into a config that may be committed.
TEST_F(McpCliTest, DisableWritesTheStateFileNotTheConfig) {
  ASSERT_EQ(run({"mcp", "add", "-s", "shared", "srv", "--", "cmd"}).status, 0);
  const util::JsonValue before = read_json(mPaths.shared_mcp_config());

  ASSERT_EQ(run({"mcp", "disable", "srv"}).status, 0);
  EXPECT_EQ(read_json(mPaths.shared_mcp_config()), before);
  mcp::McpState state(mPaths.mcp_state());
  state.load();
  EXPECT_EQ(state.enabled("srv"), std::optional<bool>(false));

  ASSERT_EQ(run({"mcp", "enable", "srv"}).status, 0);
  state.load();
  EXPECT_EQ(state.enabled("srv"), std::optional<bool>(true));

  EXPECT_EQ(run({"mcp", "disable", "nope"}).status, 1);
}

TEST_F(McpCliTest, ApproveNeedsATerminalAndShowsWhatItApproves) {
  ASSERT_EQ(run({"mcp", "add", "srv", "--", "cmd", "--flag"}).status, 0);

  CliRun r = run({"mcp", "approve", "srv"});
  EXPECT_EQ(r.status, 1);
  EXPECT_NE(r.err.find("terminal"), npos) << r.err;

  r = run({"mcp", "approve", "srv"}, /*interactive=*/true, false, "n\n");
  EXPECT_EQ(r.status, 0) << r.err;
  EXPECT_NE(r.out.find("cmd --flag"), npos) << r.out;
  EXPECT_FALSE(approved("srv"));

  r = run({"mcp", "approve", "srv"}, /*interactive=*/true, false, "y\n");
  EXPECT_EQ(r.status, 0) << r.err;
  EXPECT_TRUE(approved("srv"));

  r = run({"mcp", "approve", "srv"}, /*interactive=*/true);
  EXPECT_NE(r.out.find("already approved"), npos) << r.out;

  EXPECT_EQ(run({"mcp", "approve"}, true).status, 1);
  EXPECT_EQ(run({"mcp", "approve", "nope"}, true).status, 1);
}

TEST_F(McpCliTest, ApproveAllAsksAboutEachWaitingServer) {
  ASSERT_EQ(run({"mcp", "add", "-s", "user", "mine", "--", "cmd"}).status, 0);
  ASSERT_EQ(run({"mcp", "add", "-s", "shared", "shared_one", "--", "cmd"}).status, 0);
  ASSERT_EQ(run({"mcp", "add", "project_one", "--", "cmd"}).status, 0);

  // Asked in file order — shared, then project; the user's own is not asked.
  const CliRun r = run({"mcp", "approve", "--all"}, /*interactive=*/true, false, "y\nn\n");
  ASSERT_EQ(r.status, 0) << r.err;
  EXPECT_EQ(r.out.find("'mine'"), npos) << r.out;
  EXPECT_TRUE(approved("shared_one"));
  EXPECT_FALSE(approved("project_one"));
  EXPECT_NE(r.out.find("Approved 1 of 2"), npos) << r.out;

  // Now only one is waiting.
  const CliRun again = run({"mcp", "approve", "--all"}, true, false, "y\n");
  EXPECT_EQ(again.status, 0) << again.err;
  EXPECT_TRUE(approved("project_one"));
  EXPECT_NE(run({"mcp", "approve", "--all"}, true).out.find("No MCP server is waiting"),
            npos);
}

// In a directory under $HOME with no nearer marker the workspace is $HOME, and
// its .m8/mcp.json IS the user file: written once, read once, as the user's.
TEST_F(McpCliTest, AWorkspaceAtHomeWritesTheUserFile) {
  mPaths = resolve_m8_paths((mRoot / "home").string(), (mRoot / "home").string());
  const CliRun r = run({"mcp", "add", "srv", "--", "cmd"});
  ASSERT_EQ(r.status, 0) << r.err;
  EXPECT_NE(r.out.find("user scope"), npos) << r.out;
  EXPECT_EQ(r.out.find("m8 mcp approve"), npos) << r.out;

  // And it is found, and removed, as the user's.
  const CliRun removed = run({"mcp", "remove", "-s", "user", "srv"});
  EXPECT_EQ(removed.status, 0) << removed.err;
}

TEST_F(McpCliTest, ListChecksEachServerAndNeverStartsAnUnapprovedOne) {
  ASSERT_NE(run({"mcp", "list"}).out.find("No MCP servers configured"), npos);

  ASSERT_EQ(run({"mcp", "add", "-s", "user", "fake", "--", M8_FAKE_MCP_SERVER,
                 "--era=modern"})
                .status,
            0);
  const std::string record = (mRoot / "record.txt").string();
  ASSERT_EQ(run({"mcp", "add", "waiting", "--", M8_FAKE_MCP_SERVER, "--record=" + record})
                .status,
            0);

  const CliRun r = run({"mcp", "list"});
  ASSERT_EQ(r.status, 0) << r.err;
  EXPECT_NE(r.out.find("fake  [user, stdio]  connected"), npos) << r.out;
  EXPECT_NE(r.out.find("2026-07-28"), npos) << r.out;
  EXPECT_NE(r.out.find("waiting  [project, stdio]  needs approval"), npos) << r.out;
  EXPECT_NE(r.out.find("(m8 mcp approve <name>): waiting"), npos) << r.out;
  EXPECT_FALSE(fs::exists(record));
}

TEST_F(McpCliTest, GetShowsTheEntryMaskedAndWhatTheServerOffers) {
  ASSERT_EQ(run({"mcp", "add", "-s", "user", "-e", "TOKEN=hunter2", "-e",
                 "REF=${M8_UNSET_FOR_TEST:-fallback}", "fake", "--", M8_FAKE_MCP_SERVER,
                 "--era=legacy"})
                .status,
            0);
  const CliRun r = run({"mcp", "get", "fake"});
  ASSERT_EQ(r.status, 0) << r.err;
  EXPECT_EQ(r.out.find("hunter2"), npos) << r.out;
  EXPECT_NE(r.out.find("${M8_UNSET_FOR_TEST:-fallback}"), npos) << r.out;
  EXPECT_NE(r.out.find("connected"), npos) << r.out;
  EXPECT_NE(r.out.find("mcp__fake__echo"), npos) << r.out;
  EXPECT_NE(r.out.find("prompt  /mcp__fake__"), npos) << r.out;

  EXPECT_EQ(run({"mcp", "get", "nope"}).status, 1);
}

// Subcommand names are matched before positionals, so `m8 <model>` is
// untouched — and a model really named "mcp" still works with -m.
TEST_F(McpCliTest, TheModelArgumentStillWorks) {
  {
    CLI::App app{"m8"};
    std::string model;
    app.add_option("model,-m,--model", model);
    McpCli cli;
    cli.declare(app);
    app.parse("qwen3:8b", false);
    EXPECT_EQ(model, "qwen3:8b");
    EXPECT_FALSE(cli.parsed());
  }
  {
    CLI::App app{"m8"};
    std::string model;
    app.add_option("model,-m,--model", model);
    McpCli cli;
    cli.declare(app);
    app.parse("-m mcp", false);
    EXPECT_EQ(model, "mcp");
    EXPECT_FALSE(cli.parsed());
  }
}

TEST_F(McpCliTest, ASubcommandIsRequiredAndLoginIsNotHereYet) {
  EXPECT_EQ(run({"mcp"}).status, -1);
  EXPECT_EQ(run({"mcp", "login", "srv"}).status, 1);
  EXPECT_EQ(run({"mcp", "logout", "srv"}).status, 1);
}

}  // namespace
}  // namespace m8
