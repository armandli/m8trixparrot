// The deny list the file tools consult before touching anything.
//
// Two things are being tested, and the second matters more than the first. The
// easy half is that a protected path is refused. The half that decides whether
// this is worth having is that it is refused *however it is spelled* — through a
// symlink, through a symlinked parent, with `..` in the middle, with a `~` the
// shell never expanded. A guard that only catches the literal path is a guard
// against typing mistakes.
//
// Nothing here writes to a real protected location. Where a test needs a
// protected path that actually exists on disk — to make a symlink to, or to check
// that a directory was not created — it uses /etc, which exists everywhere and
// which the tests only ever read or fail to write.

#include <cstdlib>

#include <filesystem>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include <core/tools/protected_paths.h>
#include <core/tools/tools.h>

#include <tool_test_env.h>

namespace tools {
namespace {

bool denied(const std::string& path, PathAccess access) {
  return not protected_path_reason(path, access, "write").empty();
}

bool write_denied(const std::string& path) {
  return denied(path, PathAccess::Write);
}

bool read_denied(const std::string& path) {
  return denied(path, PathAccess::Read);
}

std::string home() {
  const char* value = std::getenv("HOME");
  return value != nullptr ? value : "";
}

struct ProtectedPathsTest : m8test::ToolTest {};

// ───────────────────────────── the two tiers ────────────────────────────────

TEST_F(ProtectedPathsTest, SecretsAreDeniedToWriteAndToRead) {
  ASSERT_FALSE(home().empty()) << "this suite needs HOME";
  for (const std::string& path : {home() + "/.ssh/authorized_keys",
                                  home() + "/.ssh/id_ed25519",
                                  home() + "/.aws/credentials",
                                  home() + "/.git-credentials",
                                  home() + "/.netrc",
                                  home() + "/.npmrc",
                                  home() + "/.gnupg/secring.gpg",
                                  home() + "/.config/gh/hosts.yml",
                                  home() + "/.parallel_api_key"}) {
    EXPECT_TRUE(write_denied(path)) << path;
    EXPECT_TRUE(read_denied(path)) << path;
    EXPECT_TRUE(is_protected_secret(path)) << path;
  }
}

// The distinction the two tiers exist for: reading ~/.zshrc to answer a question
// about it is useful and harmless; writing it is code execution on next login.
TEST_F(ProtectedPathsTest, ExecutionVectorsAreDeniedToWriteButNotToRead) {
  ASSERT_FALSE(home().empty()) << "this suite needs HOME";
  for (const std::string& path : {home() + "/.zshrc",
                                  home() + "/.bashrc",
                                  home() + "/.profile",
                                  home() + "/.gitconfig",
                                  std::string("/etc/hosts"),
                                  std::string("/usr/local/bin/anything")}) {
    EXPECT_TRUE(write_denied(path)) << path;
    EXPECT_FALSE(read_denied(path)) << path << " should stay readable";
    EXPECT_FALSE(is_protected_secret(path)) << path;
  }
}

TEST_F(ProtectedPathsTest, TheReasonNamesThePathTheCategoryAndTheTool) {
  const std::string reason = protected_path_reason(
      home() + "/.ssh/authorized_keys", PathAccess::Write, "write");
  EXPECT_NE(reason.find(home() + "/.ssh/authorized_keys"), std::string::npos)
      << reason;
  EXPECT_NE(reason.find("ssh keys and config"), std::string::npos) << reason;
  EXPECT_NE(reason.find("tool_write"), std::string::npos) << reason;
  // Without this the model reads "permission problem" and tries `cat >` next.
  EXPECT_NE(reason.find("no override"), std::string::npos) << reason;
  EXPECT_NE(reason.find("let them"), std::string::npos) << reason;
}

// ──────────────────── the three spellings of a home path ────────────────────
// bash expands an unquoted ~ before the tool starts, but a quoted one arrives
// verbatim, and $HOME is just as easy for a model to write.

TEST_F(ProtectedPathsTest, CatchesATildeTheShellNeverExpanded) {
  EXPECT_TRUE(write_denied("~/.ssh/authorized_keys"));
  EXPECT_TRUE(read_denied("~/.ssh/id_rsa"));
  EXPECT_TRUE(write_denied("~/.zshrc"));
}

TEST_F(ProtectedPathsTest, CatchesALiteralHomeVariable) {
  EXPECT_TRUE(write_denied("$HOME/.ssh/authorized_keys"));
  EXPECT_TRUE(write_denied("$HOME/.gitconfig"));
}

TEST_F(ProtectedPathsTest, CatchesDotDotTraversalBackIntoAProtectedDirectory) {
  EXPECT_TRUE(write_denied(home() + "/.ssh/../.ssh/authorized_keys"));
  EXPECT_TRUE(write_denied(home() + "/.config/../.ssh/authorized_keys"));
  EXPECT_TRUE(write_denied("/etc/../etc/hosts"));
}

TEST_F(ProtectedPathsTest, ExpandHomeLeavesEverythingElseAlone) {
  EXPECT_EQ(expand_home("/etc/hosts"), "/etc/hosts");
  EXPECT_EQ(expand_home("relative/path"), "relative/path");
  EXPECT_EQ(expand_home("~notauser/x"), "~notauser/x");
  EXPECT_EQ(expand_home("$HOMEBREW/x"), "$HOMEBREW/x");
  EXPECT_EQ(expand_home("~"), home());
  EXPECT_EQ(expand_home("~/x"), home() + "/x");
}

// ─────────────────────────────── symlinks ───────────────────────────────────
// The attack the resolution step exists for. /etc is the target because it
// exists on every machine and the test only needs to point at it.

TEST_F(ProtectedPathsTest, FollowsASymlinkToAProtectedFile) {
  std::error_code ec;
  std::filesystem::create_symlink("/etc/hosts", dir() / "innocent.txt", ec);
  if (ec) GTEST_SKIP() << "cannot create symlinks here: " << ec.message();

  EXPECT_TRUE(write_denied("innocent.txt"));
  EXPECT_TRUE(write_denied((dir() / "innocent.txt").string()));
}

TEST_F(ProtectedPathsTest, FollowsASymlinkedParentDirectory) {
  std::error_code ec;
  std::filesystem::create_directory_symlink("/etc", dir() / "cfg", ec);
  if (ec) GTEST_SKIP() << "cannot create symlinks here: " << ec.message();

  // The file itself need not exist — this is the write case, where it usually
  // does not.
  EXPECT_TRUE(write_denied("cfg/brand-new-file"));
  EXPECT_TRUE(write_denied("cfg/hosts"));
}

// ─────────────────────── component-wise, not substring ──────────────────────
// A string prefix test here would be a real bug: ~/.sshfoo is not ~/.ssh.

TEST_F(ProtectedPathsTest, DoesNotMatchANeighbourSharingAPrefix) {
  EXPECT_FALSE(write_denied(home() + "/.sshfoo/key"));
  EXPECT_FALSE(write_denied(home() + "/.zshrc.bak"));
  EXPECT_FALSE(write_denied(home() + "/.gitconfig-old"));
  EXPECT_FALSE(write_denied("/etcfoo/x"));
  EXPECT_FALSE(write_denied("/usrlocal/x"));
}

TEST_F(ProtectedPathsTest, GitDirectoryIsProtectedAtAnyDepth) {
  EXPECT_TRUE(write_denied("a/b/.git/config"));
  EXPECT_TRUE(write_denied("a/b/.git/hooks/pre-commit"));
  EXPECT_TRUE(write_denied(".git/HEAD"));
}

// .github is a different component, and blocking it would break ordinary work
// on any repository that has CI.
TEST_F(ProtectedPathsTest, GithubAndPlainGitDirectoriesAreNotProtected) {
  EXPECT_FALSE(write_denied(".github/workflows/ci.yml"));
  EXPECT_FALSE(write_denied("a/b/git/config"));
  EXPECT_FALSE(write_denied("src/gitignore-parser.cpp"));
  EXPECT_FALSE(write_denied(".gitignore"));
}

// Reading a repository's own .git is how plenty of tooling works, and the danger
// there is writing a hook, not reading one.
TEST_F(ProtectedPathsTest, GitDirectoryStaysReadable) {
  EXPECT_FALSE(read_denied("a/b/.git/config"));
  EXPECT_FALSE(is_protected_secret(".git/HEAD"));
}

// ───────────────────────── per-workspace secrets ─────────────────────────────
// Files every checkout has its own copy of: matched by their trailing
// components, wherever the workspace lives.

TEST_F(ProtectedPathsTest, WorkspaceApiKeyIsASecretInAnyWorkspace) {
  EXPECT_TRUE(read_denied(".m8/parallel_api_key"));
  EXPECT_TRUE(write_denied("deep/project/.m8/parallel_api_key"));
  EXPECT_TRUE(read_denied(home() + "/.m8/parallel_api_key"));
}

TEST_F(ProtectedPathsTest, McpConfigIsASecretInTheWorkspaceAndTheHome) {
  EXPECT_TRUE(read_denied(".m8/mcp.json"));
  EXPECT_TRUE(write_denied(".m8/mcp.json"));
  EXPECT_TRUE(read_denied("other/checkout/.m8/mcp.json"));
  EXPECT_TRUE(read_denied(home() + "/.m8/mcp.json"));
  EXPECT_TRUE(read_denied(home() + "/.m8/mcp_credentials.json"));
}

// Writing an approval approves a server, whose command then runs; reading the
// list of approvals gives nothing away.
TEST_F(ProtectedPathsTest, McpApprovalsAreAnExecutionVector) {
  EXPECT_TRUE(write_denied(home() + "/.m8/mcp_trust.json"));
  EXPECT_FALSE(read_denied(home() + "/.m8/mcp_trust.json"));
}

// The suffix is whole components, and only at the end: the shared .mcp.json
// (approval-gated by its hash instead) and the rest of .m8/ stay usable.
TEST_F(ProtectedPathsTest, SuffixMatchesWholeTrailingComponentsOnly) {
  EXPECT_FALSE(read_denied(".mcp.json"));
  EXPECT_FALSE(write_denied(".m8x/mcp.json"));
  EXPECT_FALSE(write_denied(".m8/mcp.json.bak"));
  EXPECT_FALSE(write_denied(".m8/mcp.json/inner"));
  EXPECT_FALSE(write_denied(".m8/config.json"));
  EXPECT_FALSE(write_denied(".m8/skills/x/SKILL.md"));
}

// ──────────────────────────── what stays allowed ────────────────────────────
// Regression guards. Each of these would break real work if it were caught.

TEST_F(ProtectedPathsTest, OrdinaryWorkIsUntouched) {
  EXPECT_FALSE(write_denied("src/main.cpp"));
  EXPECT_FALSE(write_denied("README.md"));
  EXPECT_FALSE(write_denied((dir() / "scratch.txt").string()));
  // $TMPDIR is under /var/folders on macOS, which is exactly why /var is not on
  // the list.
  EXPECT_FALSE(write_denied(
      (std::filesystem::temp_directory_path() / "x.txt").string()));
  EXPECT_FALSE(write_denied("/tmp/x.txt"));
}

// sp installs the scripts it writes into one of these (TODO items 3 and 4), so
// they have to stay writable.
TEST_F(ProtectedPathsTest, UserBinDirectoriesStayWritable) {
  EXPECT_FALSE(write_denied(home() + "/bin/myscript"));
  EXPECT_FALSE(write_denied(home() + "/.local/bin/myscript"));
  EXPECT_FALSE(write_denied(home() + "/.local/share/sp/bin/myscript"));
}

TEST_F(ProtectedPathsTest, PseudoDevicesStayWritableButRealDevicesDoNot) {
  EXPECT_FALSE(write_denied("/dev/null"));
  EXPECT_FALSE(write_denied("/dev/stdout"));
  EXPECT_FALSE(write_denied("/dev/stderr"));
  EXPECT_FALSE(write_denied("/dev/fd/3"));
  EXPECT_TRUE(write_denied("/dev/disk0"));
}

// An empty path is the tool's own argument error, with better wording than a
// guard could manage. Saying anything here would only bury it.
TEST_F(ProtectedPathsTest, AnEmptyPathIsLeftToTheToolToReport) {
  EXPECT_FALSE(write_denied(""));
  EXPECT_FALSE(read_denied(""));
  EXPECT_FALSE(is_protected_secret(""));
}

// ─────────────────── the guard against the tools themselves ─────────────────

TEST_F(ProtectedPathsTest, WriteToolRefusesAProtectedPath) {
  const ToolResult result = WriteTool().execute(
      m8test::args({{"path", m8test::str("/etc/m8-should-not-exist")},
                    {"content", m8test::str("x")}}));
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("protected path"), std::string::npos)
      << result.error;
  EXPECT_NE(result.error.find("tool_write"), std::string::npos) << result.error;
}

// WriteTool creates missing parent directories, which is a tested guarantee
// elsewhere in the suite. The guard has to run first, or a refused write still
// leaves the tree it would have needed behind it.
TEST_F(ProtectedPathsTest, ARefusedWriteCreatesNoDirectories) {
  const ToolResult result = WriteTool().execute(
      m8test::args({{"path", m8test::str("/etc/m8-nope/deeper/file.txt")},
                    {"content", m8test::str("x")}}));
  ASSERT_FALSE(result.ok);
  EXPECT_FALSE(std::filesystem::exists("/etc/m8-nope"))
      << "the guard ran after create_directories";
}

TEST_F(ProtectedPathsTest, EditToolRefusesAProtectedPathWithoutReadingIt) {
  const ToolResult result = EditTool().execute(m8test::args(
      {{"path", m8test::str("/etc/hosts")},
       {"edits", m8test::edits({{"localhost", "evilhost"}})}}));
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("protected path"), std::string::npos)
      << result.error;
  EXPECT_NE(result.error.find("tool_edit"), std::string::npos) << result.error;
}

TEST_F(ProtectedPathsTest, ReadToolRefusesASecretButNotAnExecutionVector) {
  const ToolResult secret = ReadTool().execute(
      m8test::args({{"path", m8test::str(home() + "/.ssh/id_rsa")}}));
  EXPECT_FALSE(secret.ok);
  EXPECT_NE(secret.error.find("protected path"), std::string::npos)
      << secret.error;

  // /etc/hosts exists on every machine this builds on and is not a secret.
  const ToolResult vector =
      ReadTool().execute(m8test::args({{"path", m8test::str("/etc/hosts")}}));
  EXPECT_TRUE(vector.ok) << vector.error;
}

// The refusal must not double as an existence oracle: it comes before the
// exists() check, so a present and an absent secret answer identically.
TEST_F(ProtectedPathsTest, ReadRefusalDoesNotRevealWhetherTheFileExists) {
  const ToolResult present = ReadTool().execute(
      m8test::args({{"path", m8test::str(home() + "/.ssh")}}));
  const ToolResult absent = ReadTool().execute(m8test::args(
      {{"path", m8test::str(home() + "/.ssh/definitely-not-here-12345")}}));
  EXPECT_FALSE(present.ok);
  EXPECT_FALSE(absent.ok);
  EXPECT_NE(present.error.find("protected path"), std::string::npos);
  EXPECT_NE(absent.error.find("protected path"), std::string::npos);
  EXPECT_EQ(absent.error.find("no such file"), std::string::npos)
      << absent.error;
}

// ───────────────────────── grep and find filtering ──────────────────────────

// Pointed at a secret, these refuse: skipping every file and reporting no
// matches would read as "nothing there" rather than "not allowed to look".
TEST_F(ProtectedPathsTest, GrepAndFindRefuseASecretSearchRoot) {
  const ToolResult grepped = GrepTool().execute(
      m8test::args({{"pattern", m8test::str("PRIVATE KEY")},
                    {"path", m8test::str(home() + "/.ssh")}}));
  EXPECT_FALSE(grepped.ok);
  EXPECT_NE(grepped.error.find("protected path"), std::string::npos)
      << grepped.error;

  const ToolResult found = FindTool().execute(
      m8test::args({{"pattern", m8test::str("*")},
                    {"path", m8test::str(home() + "/.ssh")}}));
  EXPECT_FALSE(found.ok);
  EXPECT_NE(found.error.find("protected path"), std::string::npos)
      << found.error;
}

// Crossing a secret on the way is different from being aimed at one: the walk
// must skip it and carry on, not fail.
//
// This checks the "carry on" half only, and the name says so. Building a real
// secret to walk past would mean either touching the developer's actual $HOME or
// symlinking to a root-owned file that grep could not read anyway — so which
// files get skipped is left to is_protected_secret's own tests above, and this
// pins that a walk containing .git (protected, though not a secret) and an
// ordinary file still returns the ordinary file.
TEST_F(ProtectedPathsTest, AWalkPastProtectedEntriesStillReturnsTheRest) {
  init_git_repo();
  write_file("normal.txt", "findme here\n");
  write_file("nested/also.txt", "findme too\n");

  const ToolResult result = GrepTool().execute(
      m8test::args({{"pattern", m8test::str("findme")}}));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_NE(result.output.find("normal.txt"), std::string::npos)
      << result.output;
  EXPECT_NE(result.output.find("also.txt"), std::string::npos) << result.output;
  // .git is right there in the walk and contributes nothing to the output.
  EXPECT_EQ(result.output.find(".git/"), std::string::npos) << result.output;
}

}  // namespace
}  // namespace tools
