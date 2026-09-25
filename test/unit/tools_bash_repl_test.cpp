// What `bash_repl` does that `bash` cannot: state survives between calls.
//
// It keeps BashTool's contract otherwise — ok == true for any command it
// managed to start, with a non-zero exit, a timeout, or a lost session
// reported as text in the output rather than as a tool failure. The tests that
// matter most are the ones about what happens when things go wrong, because a
// persistent shell has state to lose and losing it silently would be worse
// than not having it.

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include <core/tools/bash_repl.h>
#include <core/tools/tools.h>
#include <tool_test_env.h>

namespace m8test {
namespace {

namespace sc = std::chrono;

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// One session per test, so nothing leaks between them.
struct BashReplTest : ToolTest {
  tools::BashReplSession session;

  tools::ToolResult run(const std::string& command) {
    return tools::BashReplTool{session}.execute(args({{"command", str(command)}}));
  }
};

// ─────────────────────── the reason this tool exists ───────────────────────

TEST_F(BashReplTest, AVariableSetInOneCallIsStillSetInTheNext) {
  const tools::ToolResult first = run("X=42");
  EXPECT_TRUE(first.ok) << first.error;

  const tools::ToolResult second = run("echo $X");
  EXPECT_TRUE(second.ok) << second.error;
  EXPECT_TRUE(has(second.output, "42")) << second.output;
}

TEST_F(BashReplTest, ChangingDirectorySticks) {
  run("mkdir -p sub/dir");
  const tools::ToolResult moved = run("cd sub/dir");
  EXPECT_TRUE(moved.ok);

  const tools::ToolResult where = run("pwd");
  EXPECT_TRUE(has(where.output, "sub/dir")) << where.output;
  // The reported cwd agrees with the shell's own idea of it.
  EXPECT_TRUE(has(where.output, "[cwd: ")) << where.output;
}

TEST_F(BashReplTest, FunctionsAndExportsSurvive) {
  run("greet() { echo hello-$1; }");
  run("export M8_REPL_VAR=exported");

  const tools::ToolResult called = run("greet world");
  EXPECT_TRUE(has(called.output, "hello-world")) << called.output;

  // An export reaches a *child* process, not just the shell itself.
  const tools::ToolResult child = run("bash -c 'echo $M8_REPL_VAR'");
  EXPECT_TRUE(has(child.output, "exported")) << child.output;
}

// ────────────────────────── BashTool's contract ────────────────────────────

TEST_F(BashReplTest, RunsACommandAndReturnsItsStdout) {
  const tools::ToolResult result = run("echo hello");
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
  EXPECT_TRUE(has(result.output, "hello")) << result.output;
  EXPECT_FALSE(result.truncated);
}

// Note `(exit 3)` rather than `exit 3`: in a shell that persists, a bare
// `exit` ends the session rather than the command, which is what
// AShellThatExitsIsReplacedAndSaidSo covers.
TEST_F(BashReplTest, NonZeroExitIsStillOkAndReportsTheStatus) {
  const tools::ToolResult result = run("(exit 3)");
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
  EXPECT_TRUE(has(result.output, "[command exited with status 3]"))
      << result.output;
}

TEST_F(BashReplTest, AFailedCommandReportsItsStatusAndKeepsTheSession) {
  run("STAYS=put");

  const tools::ToolResult failed = run("ls /definitely/not/here");
  EXPECT_TRUE(failed.ok) << failed.error;
  EXPECT_TRUE(has(failed.output, "[command exited with status ")) << failed.output;

  const tools::ToolResult after = run("echo $STAYS");
  EXPECT_TRUE(has(after.output, "put")) << after.output;
}

TEST_F(BashReplTest, StderrIsMergedIntoStdout) {
  const tools::ToolResult result = run("echo out; echo err >&2");
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(has(result.output, "out")) << result.output;
  EXPECT_TRUE(has(result.output, "err")) << result.output;
}

TEST_F(BashReplTest, MissingCommandIsAnError) {
  const tools::ToolResult result = tools::BashReplTool{session}.execute(args({}));
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(has(result.error, "command")) << result.error;
}

// ──────────────────────── the protocol stays hidden ────────────────────────

// The end marker is how a command's output is delimited; if any of it reached
// the model the output would be a lie.
TEST_F(BashReplTest, TheMarkerNeverAppearsInTheOutput) {
  const tools::ToolResult result = run("echo plain");
  EXPECT_FALSE(has(result.output, "__M8_END_")) << result.output;
  EXPECT_FALSE(has(result.output, "__M8_CWD")) << result.output;
}

// A command is free to print something marker-shaped. Only the session's own
// token ends a command, so this is passed through untouched.
TEST_F(BashReplTest, OutputThatLooksLikeAMarkerIsNotMistakenForOne) {
  const tools::ToolResult result = run("echo '__M8_END_not-the-token__0__/nowhere'");
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(has(result.output, "__M8_END_not-the-token__0__/nowhere"))
      << result.output;
}

// A command is staged in a temp file the shell sources, so bash blames that
// path on a parse error. The model can do nothing with a temp path.
TEST_F(BashReplTest, ASyntaxErrorIsReportedWithoutLeakingTheTempPath) {
  const tools::ToolResult result = run("echo \"unterminated");
  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(has(result.output, "unexpected EOF")) << result.output;
  EXPECT_FALSE(has(result.output, "m8-bash-repl-")) << result.output;

  // And the session is still usable afterwards — the half-parsed command did
  // not swallow the protocol.
  const tools::ToolResult after = run("echo survived");
  EXPECT_TRUE(has(after.output, "survived")) << after.output;
}

// ───────────────────────────── losing state ────────────────────────────────

TEST_F(BashReplTest, ATimeoutInterruptsTheCommandAndKeepsTheSession) {
  run("KEEP=me");

  const auto began = sc::steady_clock::now();
  const tools::ToolResult slow = tools::BashReplTool{session}.execute(
      args({{"command", str("sleep 30")}, {"timeout", num(1)}}));
  const auto took = sc::steady_clock::now() - began;

  EXPECT_TRUE(slow.ok) << slow.error;
  EXPECT_TRUE(has(slow.output, "timed out")) << slow.output;
  EXPECT_TRUE(has(slow.output, "session state preserved")) << slow.output;
  EXPECT_LT(sc::duration_cast<sc::seconds>(took).count(), 10);

  // The half of the promise that matters: interrupting cost nothing.
  const tools::ToolResult after = run("echo $KEEP");
  EXPECT_TRUE(has(after.output, "me")) << after.output;
}

TEST_F(BashReplTest, RestartClearsTheSession) {
  run("GONE=soon");

  const tools::ToolResult reset = tools::BashReplTool{session}.execute(
      args({{"restart", flag(true)}}));
  EXPECT_TRUE(reset.ok);
  EXPECT_TRUE(has(reset.output, "restarted")) << reset.output;

  const tools::ToolResult after = run("echo \"[$GONE]\"");
  EXPECT_TRUE(has(after.output, "[]")) << after.output;
}

// `exit` kills the shell. The tool has to notice, start a new one, and say so
// rather than silently handing back an empty result forever.
TEST_F(BashReplTest, AShellThatExitsIsReplacedAndSaidSo) {
  run("VANISHES=yes");

  const tools::ToolResult gone = run("exit");
  EXPECT_TRUE(gone.ok) << gone.error;
  EXPECT_TRUE(has(gone.output, "the shell exited")) << gone.output;

  const tools::ToolResult after = run("echo \"[$VANISHES]\" recovered");
  EXPECT_TRUE(after.ok) << after.error;
  EXPECT_TRUE(has(after.output, "recovered")) << after.output;
  EXPECT_TRUE(has(after.output, "[]")) << after.output;
}

// ───────────────────────────── background work ─────────────────────────────

TEST_F(BashReplTest, ABackgroundJobDoesNotBlockTheCall) {
  const auto began = sc::steady_clock::now();
  const tools::ToolResult result = run("sleep 5 > /dev/null 2>&1 &");
  const auto took = sc::steady_clock::now() - began;

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_LT(sc::duration_cast<sc::seconds>(took).count(), 3);
}

TEST_F(BashReplTest, WaitCollectsABackgroundJob) {
  run("(sleep 0.2; echo done > bg.out) &");
  const tools::ToolResult waited = run("wait; cat bg.out");
  EXPECT_TRUE(has(waited.output, "done")) << waited.output;
}

// ─────────────────────────────── big output ────────────────────────────────

TEST_F(BashReplTest, LargeOutputIsTruncatedLikeBash) {
  const tools::ToolResult result = run("seq 1 200000");
  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.truncated);
  EXPECT_FALSE(result.overflow_path.empty());
  EXPECT_TRUE(has(result.output, "output truncated")) << "missing note";
}

}  // namespace
}  // namespace m8test
