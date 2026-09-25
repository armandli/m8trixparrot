// Tests for run_shell_capture's timeout overload — the bound that keeps a
// wedged subprocess from hanging the tool that shelled out to it.

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include <core/tools/tools_util.h>

namespace tools {
namespace {

using Clock = std::chrono::steady_clock;

// The assertions below are deliberately loose: they separate "the deadline
// held" from "the command ran to completion", which differ by a minute here,
// rather than trying to measure the deadline itself on a loaded machine.
double elapsed_seconds(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

TEST(RunShellCaptureTimeout, FastCommandReturnsWithoutWaitingOutTheDeadline) {
  const auto start = Clock::now();
  const std::string output = run_shell_capture("echo hello", 60);

  EXPECT_EQ(output, "hello\n");
  // A watchdog left holding the capture pipe would stall this for its full
  // sleep instead of returning as soon as the command exits.
  EXPECT_LT(elapsed_seconds(start), 20.0);
}

TEST(RunShellCaptureTimeout, SlowCommandIsKilledAndPartialOutputKept) {
  // `sleep` runs as a child of the shell, which is the shape that wedged
  // bash_search's PATH scan: killing only the shell leaves the child alive and
  // holding the capture pipe open, so the read blocks for the full sleep.
  const auto start = Clock::now();
  const std::string output =
      run_shell_capture("echo before; sleep 90; echo after", 2);

  EXPECT_LT(elapsed_seconds(start), 30.0);
  EXPECT_NE(output.find("before"), std::string::npos);
  EXPECT_EQ(output.find("after"), std::string::npos);
}

TEST(RunShellCaptureTimeout, CommandStderrStillReachesItsOwnRedirection) {
  // The wrapper silences its own stderr; the command's must survive, or a
  // caller's `2>&1` would quietly capture nothing.
  EXPECT_EQ(run_shell_capture("{ echo oops >&2; } 2>&1", 60), "oops\n");
}

TEST(RunShellCaptureTimeout, WatchdogIsGoneOnceTheCommandFinishes) {
  // The watchdog outlives a command that finishes early unless it is reaped,
  // and it inherits the caller's descriptors: leave it running and a caller
  // piping this process's stdout onward watches that pipe stay open for the
  // whole timeout even though the command returned at once.
  //
  // 91 is just an unusual enough sleep length to look for afterwards, and the
  // bracket in the pattern keeps pgrep from matching its own command line.
  constexpr int kDistinctiveTimeout = 91;
  EXPECT_EQ(run_shell_capture("echo quick", kDistinctiveTimeout), "quick\n");

  const std::string leftover =
      run_shell_capture("pgrep -f 'sl[e]ep 91' 2>/dev/null | wc -l | tr -d ' \n'");
  // "0" whether nothing matched or the system has no pgrep at all.
  EXPECT_EQ(leftover, "0") << "a watchdog sleep outlived its capture";
}

TEST(RunShellCaptureTimeout, NonPositiveTimeoutMeansNoLimit) {
  EXPECT_EQ(run_shell_capture("echo hello", 0), "hello\n");
  EXPECT_EQ(run_shell_capture("echo hello", -1), "hello\n");
}

}  // namespace
}  // namespace tools
