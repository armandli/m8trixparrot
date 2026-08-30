// ShellSession drives a real interactive shell on a pty. These tests run it
// headless: forkpty works without a controlling terminal, and the shell reads
// the commands written to its master fd. They are timing-tolerant (poll with a
// deadline) rather than assuming a fixed latency.

#include <core/shell_session.h>

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

namespace agent {
namespace {

namespace sc = std::chrono;

struct Collector {
  std::mutex mutex;
  std::string data;
  bool exited = false;
  int status = 0;

  void bytes(std::string_view b) {
    std::lock_guard<std::mutex> lock(mutex);
    data.append(b);
  }
  std::string snapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    return data;
  }
};

bool wait_for_output(Collector& c, const std::string& needle,
                     sc::milliseconds timeout) {
  const auto deadline = sc::steady_clock::now() + timeout;
  while (sc::steady_clock::now() < deadline) {
    if (c.snapshot().find(needle) != std::string::npos) return true;
    std::this_thread::sleep_for(sc::milliseconds(20));
  }
  return c.snapshot().find(needle) != std::string::npos;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() and
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

TEST(ShellSessionTest, RunsACommandAndStreamsItsOutput) {
  Collector c;
  ShellSession sh;
  sh.on_bytes = [&c](std::string_view b) { c.bytes(b); };

  std::string error;
  ASSERT_TRUE(sh.start(80, 24, "", &error)) << error;

  sh.write_bytes("printf 'MARK-%s\\n' one\n");
  EXPECT_TRUE(wait_for_output(c, "MARK-one", sc::seconds(5))) << c.snapshot();
}

TEST(ShellSessionTest, CwdTracksTheShellsChdir) {
  Collector c;
  ShellSession sh;
  sh.on_bytes = [&c](std::string_view b) { c.bytes(b); };

  std::string error;
  ASSERT_TRUE(sh.start(80, 24, "", &error)) << error;

  sh.write_bytes("cd /tmp\n");

  bool ok = false;
  const auto deadline = sc::steady_clock::now() + sc::seconds(5);
  while (sc::steady_clock::now() < deadline) {
    // /tmp is a symlink to /private/tmp on macOS, so match either.
    if (ends_with(sh.cwd(), "/tmp")) {
      ok = true;
      break;
    }
    std::this_thread::sleep_for(sc::milliseconds(50));
  }
  EXPECT_TRUE(ok) << "cwd() = '" << sh.cwd() << "'";
}

TEST(ShellSessionTest, StaysResponsiveAfterCtrlC) {
  Collector c;
  ShellSession sh;
  sh.on_bytes = [&c](std::string_view b) { c.bytes(b); };

  std::string error;
  ASSERT_TRUE(sh.start(80, 24, "", &error)) << error;

  sh.write_bytes("cat\n");  // blocks reading stdin
  std::this_thread::sleep_for(sc::milliseconds(300));
  sh.write_bytes("\x03");  // Ctrl-C: SIGINT to the foreground job
  std::this_thread::sleep_for(sc::milliseconds(300));
  sh.write_bytes("printf 'BACK\\n'\n");

  EXPECT_TRUE(wait_for_output(c, "BACK", sc::seconds(5))) << c.snapshot();
}

TEST(ShellSessionTest, ExtraEnvReachesTheShell) {
  Collector c;
  ShellSession sh;
  sh.on_bytes = [&c](std::string_view b) { c.bytes(b); };

  std::string error;
  ASSERT_TRUE(sh.start(80, 24, "", &error, {{"M8_TEST_VAR", "hello-42"}}))
      << error;

  sh.write_bytes("printf 'GOT=[%s]\\n' \"$M8_TEST_VAR\"\n");
  EXPECT_TRUE(wait_for_output(c, "GOT=[hello-42]", sc::seconds(5)))
      << c.snapshot();
}

TEST(ShellSessionTest, ResizePropagatesToTheChild) {
  Collector c;
  ShellSession sh;
  sh.on_bytes = [&c](std::string_view b) { c.bytes(b); };

  std::string error;
  ASSERT_TRUE(sh.start(80, 24, "", &error)) << error;

  sh.resize(120, 40);
  std::this_thread::sleep_for(sc::milliseconds(100));
  sh.write_bytes("stty size\n");

  EXPECT_TRUE(wait_for_output(c, "40 120", sc::seconds(5))) << c.snapshot();
}

TEST(ShellSessionTest, OnExitFiresWhenTheShellQuits) {
  Collector c;
  ShellSession sh;
  sh.on_bytes = [&c](std::string_view b) { c.bytes(b); };
  sh.on_exit = [&c](int status) {
    std::lock_guard<std::mutex> lock(c.mutex);
    c.exited = true;
    c.status = status;
  };

  std::string error;
  ASSERT_TRUE(sh.start(80, 24, "", &error)) << error;

  sh.write_bytes("exit 0\n");

  bool exited = false;
  const auto deadline = sc::steady_clock::now() + sc::seconds(5);
  while (sc::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(c.mutex);
      exited = c.exited;
    }
    if (exited) break;
    std::this_thread::sleep_for(sc::milliseconds(50));
  }
  EXPECT_TRUE(exited);
  EXPECT_FALSE(sh.running());
}

TEST(ShellSessionTest, DestructorTearsDownAStillRunningShell) {
  {
    ShellSession sh;
    std::string error;
    ASSERT_TRUE(sh.start(80, 24, "", &error)) << error;
    sh.write_bytes("sleep 60\n");
    std::this_thread::sleep_for(sc::milliseconds(200));
    // ~ShellSession here: SIGHUP then SIGKILL, join the reader. Must not hang.
  }
  SUCCEED();
}

}  // namespace
}  // namespace agent
