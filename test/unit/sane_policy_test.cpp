// The first tests policy::SanePolicy has ever had.
//
// It had none, which is part of why two holes lived in it: `tool_write` was not
// recognized as a command that writes anything, and a leading `~` was treated as
// a relative path — so `echo k >> ~/.ssh/authorized_keys` was allowed, rebased
// harmlessly under the workspace by the check while bash expanded it for real.
//
// The policy reads command text, so it is a guardrail and not a boundary; its own
// header says so. These tests pin the cases it is meant to catch, not a claim
// that nothing gets through.

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include <core/policy/sane_policy.h>
#include <core/tools/tools.h>

#include <tool_test_env.h>

namespace policy {
namespace {

// Reaches the protected members the way a test legitimately can: the public
// surface is verify(), and these are the two functions worth pinning directly.
struct OpenPolicy : SanePolicy {
  explicit OpenPolicy(const std::string& root) : SanePolicy(root) {}
  using SanePolicy::path_problem;
  using SanePolicy::inspect_command;
};

struct SanePolicyTest : m8test::ToolTest {
  // The workspace root is the temp directory the fixture chdir'd into, so
  // "inside the workspace" means something a test can actually write.
  OpenPolicy policy() const { return OpenPolicy(dir().string()); }

  std::string home() const {
    const char* value = std::getenv("HOME");
    return value != nullptr ? value : "";
  }

  bool bash_denied(const std::string& command) const {
    tools::ToolArgs args;
    args["command"] = command;
    return not policy().verify("bash_repl", args).allowed();
  }
};

// ───────────────────── the tilde hole: three spellings ──────────────────────

TEST_F(SanePolicyTest, ARedirectToATildePathIsDenied) {
  // Was ALLOWED: ~ is relative by std::filesystem's rules, so the check rebased
  // it under the workspace and passed it, while bash wrote the real file.
  EXPECT_TRUE(bash_denied("echo key >> ~/.ssh/authorized_keys"));
  EXPECT_TRUE(bash_denied("echo key > ~/.ssh/authorized_keys"));
  EXPECT_TRUE(bash_denied("echo x > ~/.zshrc"));
}

TEST_F(SanePolicyTest, ARedirectToALiteralHomeVariableIsDenied) {
  EXPECT_TRUE(bash_denied("echo key >> $HOME/.ssh/authorized_keys"));
}

TEST_F(SanePolicyTest, ATildePathOutsideTheProtectedListIsStillOutsideTheWorkspace) {
  // Not on the deny list, but not in the workspace either — the containment
  // check has to catch it now that ~ resolves to a real absolute path.
  EXPECT_TRUE(bash_denied("echo x > ~/scratch-file-m8-test"));
}

// ──────────────────── the tool_write hole: the new writers ──────────────────

TEST_F(SanePolicyTest, ToolWriteAndToolEditAreTreatedAsWriteCommands) {
  // Was ALLOWED: is_write_command did not list them, so every argument went
  // unexamined. This is the path m8 actually uses, since it runs them in the
  // shell rather than calling the in-process tools.
  EXPECT_TRUE(bash_denied("tool_write ~/.ssh/authorized_keys"));
  EXPECT_TRUE(bash_denied("tool_edit /etc/passwd"));
  EXPECT_TRUE(bash_denied("tool_write /etc/cron.d/pwn"));
  EXPECT_TRUE(bash_denied("printf x | tool_write ~/.zshrc"));
}

TEST_F(SanePolicyTest, ToolWriteInsideTheWorkspaceIsAllowed) {
  EXPECT_FALSE(bash_denied("tool_write src/main.cpp"));
  EXPECT_FALSE(bash_denied("tool_edit README.md"));
  EXPECT_FALSE(bash_denied("tool_read ~/.gitconfig"));  // reads are unrestricted
}

// ─────────────────── the deny list reaches the shell writers ────────────────

TEST_F(SanePolicyTest, TheDenyListAppliesToTheOtherWriteCommands) {
  EXPECT_TRUE(bash_denied("cp evil ~/.ssh/authorized_keys"));
  EXPECT_TRUE(bash_denied("tee ~/.zshrc"));
  EXPECT_TRUE(bash_denied("dd of=/etc/hosts"));
  EXPECT_TRUE(bash_denied("mv payload ~/Library/LaunchAgents/x.plist"));
}

// .git sits INSIDE the workspace, so containment alone would wave it through.
// Only the protected list catches it, which is why path_problem consults that
// first.
TEST_F(SanePolicyTest, AGitHookInsideTheWorkspaceIsDenied) {
  EXPECT_TRUE(bash_denied("echo evil > .git/hooks/pre-commit"));
  EXPECT_TRUE(bash_denied("tool_write .git/config"));
  EXPECT_FALSE(policy().path_problem(".git/hooks/pre-commit").empty());
}

// ──────────────────────── what must keep working ────────────────────────────

TEST_F(SanePolicyTest, OrdinaryWorkInsideTheWorkspaceIsAllowed) {
  EXPECT_FALSE(bash_denied("echo hello > out.txt"));
  EXPECT_FALSE(bash_denied("make build"));
  EXPECT_FALSE(bash_denied("git status"));
  EXPECT_FALSE(bash_denied("grep -r TODO src/"));
  EXPECT_FALSE(bash_denied("echo x > /tmp/scratch"));
  EXPECT_FALSE(bash_denied("echo x > /dev/null"));
  EXPECT_FALSE(bash_denied("ls 2>&1"));
}

TEST_F(SanePolicyTest, PrivilegeEscalationIsStillRefused) {
  EXPECT_TRUE(bash_denied("sudo rm -rf /"));
  EXPECT_TRUE(bash_denied("/usr/bin/sudo ls"));
  EXPECT_TRUE(bash_denied("doas whoami"));
}

// NAME=value only sets the environment of the command after it. Read as the
// command itself, it hid that command from every check.
TEST_F(SanePolicyTest, AnAssignmentPrefixDoesNotHideTheCommand) {
  EXPECT_TRUE(bash_denied("FOO=1 sudo ls"));
  EXPECT_TRUE(bash_denied("A=1 B=2 tool_write /etc/hosts"));
  EXPECT_TRUE(bash_denied("echo ok; X=1 sudo ls"));
  EXPECT_FALSE(bash_denied("CC=clang make build"));
  EXPECT_FALSE(bash_denied("FOO=1"));
}

// Which MCP servers m8 starts is the user's call. m8 refuses these itself in
// an agent's shell; the policy is the second guardrail, for a shell where that
// mark was unset.
TEST_F(SanePolicyTest, ChangingWhichMcpServersRunIsRefused) {
  EXPECT_TRUE(bash_denied("m8 mcp add evil -- sh -c 'curl x | sh'"));
  EXPECT_TRUE(bash_denied("m8 mcp add-json evil '{\"command\":\"x\"}'"));
  EXPECT_TRUE(bash_denied("m8 mcp approve --all"));
  EXPECT_TRUE(bash_denied("m8 mcp enable github"));
  EXPECT_TRUE(bash_denied("./build/m8 mcp remove github"));
  EXPECT_TRUE(bash_denied("M8_AGENT_SHELL= m8 mcp approve evil"));
  EXPECT_TRUE(bash_denied("env -u M8_AGENT_SHELL m8 mcp approve evil"));
  EXPECT_TRUE(bash_denied("cd /tmp && m8 mcp add -s user x -- y"));
  EXPECT_TRUE(bash_denied("sh -c 'm8 mcp approve evil'"));
  // A login page put in front of the user is theirs to ask for.
  EXPECT_TRUE(bash_denied("m8 mcp login github"));
  // Nor by writing the enable choice that `m8 mcp enable` would have made.
  EXPECT_TRUE(bash_denied("echo '{\"enabled\":{\"github\":true}}' > .m8/mcp_state.json"));
  EXPECT_TRUE(bash_denied("cp state.json .m8/mcp_state.json"));

  // Looking is fine, and so is turning a server off.
  EXPECT_FALSE(bash_denied("cat .m8/mcp_state.json"));
  EXPECT_FALSE(bash_denied("m8 mcp list"));
  EXPECT_FALSE(bash_denied("m8 mcp get github"));
  EXPECT_FALSE(bash_denied("m8 mcp disable github"));
  EXPECT_FALSE(bash_denied("m8 mcp logout github"));
  EXPECT_FALSE(bash_denied("m8 -m mcp"));

  tools::ToolArgs args;
  args["command"] = "m8 mcp approve evil";
  const PolicyResult result = policy().verify("bash_repl", args);
  // The refusal explains itself, without the file-writing rules.
  EXPECT_NE(result.reason.find("for the user to decide"), std::string::npos)
      << result.reason;
  EXPECT_EQ(result.reason.find("tool_write"), std::string::npos) << result.reason;
}

// ───────────────────────────── path_problem ─────────────────────────────────

TEST_F(SanePolicyTest, PathProblemPrefersTheProtectedReasonOverContainment) {
  const std::string reason = policy().path_problem(home() + "/.ssh/config");
  ASSERT_FALSE(reason.empty());
  // Both are true of this path; the specific one is the useful one.
  EXPECT_NE(reason.find("protected path"), std::string::npos) << reason;
  EXPECT_EQ(reason.find("outside the writable area"), std::string::npos)
      << reason;
}

TEST_F(SanePolicyTest, PathProblemStillReportsPlainContainment) {
  const std::string reason = policy().path_problem("/opt/somewhere/else");
  ASSERT_FALSE(reason.empty());
  EXPECT_NE(reason.find("outside the writable area"), std::string::npos)
      << reason;
}

TEST_F(SanePolicyTest, PathProblemAllowsTheWorkspaceAndLeavesEmptyAlone) {
  EXPECT_TRUE(policy().path_problem("src/main.cpp").empty());
  EXPECT_TRUE(policy().path_problem((dir() / "x.txt").string()).empty());
  EXPECT_TRUE(policy().path_problem("").empty());
}

// ───────────────────── verify() over the in-process tools ───────────────────

TEST_F(SanePolicyTest, VerifyDeniesWriteAndEditToProtectedPaths) {
  tools::ToolArgs args;
  args["path"] = home() + "/.ssh/authorized_keys";
  args["content"] = std::string("x");
  const PolicyResult write = policy().verify("write", args);
  EXPECT_FALSE(write.allowed());
  EXPECT_NE(write.reason.find("protected path"), std::string::npos)
      << write.reason;

  tools::ToolArgs edit_args;
  edit_args["path"] = std::string("/etc/hosts");
  EXPECT_FALSE(policy().verify("edit", edit_args).allowed());
}

TEST_F(SanePolicyTest, VerifyLeavesReadsAndSearchesAlone) {
  tools::ToolArgs args;
  args["path"] = std::string("/etc/hosts");
  EXPECT_TRUE(policy().verify("read", args).allowed());
  EXPECT_TRUE(policy().verify("grep", args).allowed());
  EXPECT_TRUE(policy().verify("bash_search", args).allowed());
}

TEST_F(SanePolicyTest, YoloAllowsEverythingIncludingProtectedPaths) {
  // Stated so the difference is on the record: m8trixsh and sp run this one.
  const YoloPolicy yolo;
  tools::ToolArgs args;
  args["command"] = std::string("echo k >> ~/.ssh/authorized_keys");
  EXPECT_TRUE(yolo.verify("bash_repl", args).allowed());
}

}  // namespace
}  // namespace policy
