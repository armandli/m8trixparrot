// What `bash` does, and what it deliberately does not do.
//
// The contract worth internalizing: BashTool reports ok == true for any
// command it managed to *start*, whatever that command then did. A non-zero
// exit, a signal, a timeout — all of those are content in the output, not tool
// failures. Only failing to launch the command is an error.

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include <core/tools.h>
#include <tool_test_env.h>

namespace agent::test {
namespace {

struct BashTest : ToolTest {};

TEST_F(BashTest, RunsACommandAndReturnsItsStdout) {
  const ToolResult result = BashTool().execute(args({{"command", str("echo hello")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "hello\n");
  EXPECT_TRUE(result.error.empty());
  EXPECT_FALSE(result.truncated);
}

// The headline surprise. A caller checking only `ok` will treat a failed
// command as a success; the status is recoverable *only* by reading the text.
TEST_F(BashTest, NonZeroExitIsStillOkAndReportsTheStatusInTheOutput) {
  const ToolResult result = BashTool().execute(args({{"command", str("exit 3")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
  EXPECT_EQ(result.output, "\n[command exited with status 3]");
}

TEST_F(BashTest, StderrIsMergedIntoStdout) {
  const ToolResult result =
      BashTool().execute(args({{"command", str("echo out; echo err >&2")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_NE(result.output.find("out"), std::string::npos);
  EXPECT_NE(result.output.find("err"), std::string::npos);
}

// Limitation: exit_note() has a "[command killed by signal N]" branch, but it
// is unreachable in practice. popen() runs the invocation through an
// intermediate /bin/sh, and that shell exits *normally* with 128+N when its
// child dies on a signal — so pclose() reports WIFEXITED, never WIFSIGNALED.
// A SIGTERM therefore surfaces as status 143, and the caller has to know the
// 128+N convention to read it.
TEST_F(BashTest, ACommandKilledBySignalIsReportedAsExitStatus128PlusTheSignal) {
  const ToolResult result =
      BashTool().execute(args({{"command", str("kill -TERM $$")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_NE(result.output.find("[command exited with status 143]"),
            std::string::npos);
  EXPECT_EQ(result.output.find("killed by signal"), std::string::npos);
}

TEST_F(BashTest, TimeoutKillsALongCommandAndNamesTheTimeout) {
  const auto started = std::chrono::steady_clock::now();
  const ToolResult result = BashTool().execute(
      args({{"command", str("sleep 30")}, {"timeout", num(1)}}));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(result.ok);
  EXPECT_NE(result.output.find("[command timed out after 1s]"),
            std::string::npos);
  // It really was killed, not waited out.
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 25);
}

// A timeout of zero means "no timeout", not "give up immediately" — the
// argument is only honored when positive.
TEST_F(BashTest, ATimeoutOfZeroIsIgnoredRatherThanMeaningImmediately) {
  const ToolResult result = BashTool().execute(
      args({{"command", str("echo survived")}, {"timeout", num(0)}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "survived\n");
}

// The command is handed to `bash -c` inside single quotes, so an embedded
// single quote has to be escaped by closing and reopening the quote.
TEST_F(BashTest, ACommandContainingSingleQuotesIsQuotedCorrectly) {
  const ToolResult result = BashTool().execute(
      args({{"command", str("echo \"it's fine\"")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "it's fine\n");
}

// Limitation: there is no working-directory argument. The command runs
// wherever the process happens to be, which for these tests is the fixture's
// temp directory. A caller wanting a different directory has to `cd` inside
// the command itself.
TEST_F(BashTest, TheCommandInheritsTheProcessWorkingDirectory) {
  write_file("marker.txt", "x");

  const ToolResult result = BashTool().execute(args({{"command", str("ls")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "marker.txt\n");
}

TEST_F(BashTest, MissingCommandIsAnError) {
  const ToolResult result = BashTool().execute(args({}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "bash: missing required string argument 'command'");
  EXPECT_TRUE(result.output.empty());
}

TEST_F(BashTest, AnEmptyCommandIsTreatedAsMissing) {
  const ToolResult result = BashTool().execute(args({{"command", str("")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "bash: missing required string argument 'command'");
}

// An argument of the wrong JSON type reads as absent rather than as a type
// error — string_arg() returns nullopt for both cases (see tools_util.h).
TEST_F(BashTest, ACommandOfTheWrongTypeReadsAsMissing) {
  const ToolResult result = BashTool().execute(args({{"command", num(42)}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "bash: missing required string argument 'command'");
}

// Output past 5000 lines is cut and the remainder written to a temp file, with
// a note pointing at it. Every tool inherits this cap from truncate_output().
TEST_F(BashTest, OutputBeyondFiveThousandLinesIsTruncatedToATempFile) {
  const ToolResult result =
      BashTool().execute(args({{"command", str("seq 1 6000")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.truncated);
  EXPECT_FALSE(result.overflow_path.empty());
  EXPECT_NE(result.output.find("[output truncated; full output saved to"),
            std::string::npos);
  // The kept portion stops at the cap, so line 6000 is not in it.
  EXPECT_EQ(result.output.find("\n6000\n"), std::string::npos);

  std::error_code ec;
  std::filesystem::remove(result.overflow_path, ec);
}

}  // namespace
}  // namespace agent::test
