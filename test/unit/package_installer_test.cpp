// PackageInstaller's dedup queue: N callers asking for the same package at
// once must produce exactly one Runner invocation, and every caller gets that
// one result. Real pip is never exercised here — set_runner_for_test swaps in
// a counting stub, since this is testing the queueing/dedup logic, not pip
// itself.
//
// PackageInstaller is a process-wide singleton, and its "already installed"
// memoization persists for the life of the test binary, so each test uses its
// own package name to avoid bleeding state into other tests.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <core/package_installer.h>

namespace agent {
namespace {

TEST(PackageInstallerTest, ConcurrentRequestsForSamePackageDedupToOneRun) {
  std::atomic<int> calls{0};
  PackageInstaller::set_runner_for_test([&calls](const std::string& package) {
    calls.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    PackageInstallResult result;
    result.ok = true;
    result.output = "installed " + package;
    return result;
  });

  constexpr int kCallers = 8;
  std::vector<std::thread> threads;
  std::atomic<int> successes{0};
  for (int i = 0; i < kCallers; ++i) {
    threads.emplace_back([&] {
      const PackageInstallResult result =
          PackageInstaller::instance().install("dedup-test-package");
      if (result.ok) successes.fetch_add(1);
    });
  }
  for (std::thread& t : threads) t.join();

  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(successes.load(), kCallers);
}

TEST(PackageInstallerTest, SecondRequestAfterSuccessSkipsTheRunner) {
  std::atomic<int> calls{0};
  PackageInstaller::set_runner_for_test([&calls](const std::string&) {
    calls.fetch_add(1);
    PackageInstallResult result;
    result.ok = true;
    return result;
  });

  const PackageInstallResult first =
      PackageInstaller::instance().install("memoized-test-package");
  const PackageInstallResult second =
      PackageInstaller::instance().install("memoized-test-package");

  EXPECT_TRUE(first.ok);
  EXPECT_FALSE(first.already_installed);
  EXPECT_TRUE(second.ok);
  EXPECT_TRUE(second.already_installed);
  EXPECT_EQ(calls.load(), 1);
}

TEST(PackageInstallerTest, DistinctPackagesEachGetTheirOwnRun) {
  std::atomic<int> calls{0};
  PackageInstaller::set_runner_for_test([&calls](const std::string&) {
    calls.fetch_add(1);
    PackageInstallResult result;
    result.ok = true;
    return result;
  });

  const PackageInstallResult a =
      PackageInstaller::instance().install("distinct-test-package-a");
  const PackageInstallResult b =
      PackageInstaller::instance().install("distinct-test-package-b");

  EXPECT_TRUE(a.ok);
  EXPECT_TRUE(b.ok);
  EXPECT_EQ(calls.load(), 2);
}

TEST(PackageInstallerTest, FailedInstallIsNotMemoizedAsInstalled) {
  std::atomic<int> calls{0};
  PackageInstaller::set_runner_for_test([&calls](const std::string&) {
    calls.fetch_add(1);
    PackageInstallResult result;
    result.ok = false;
    result.error = "no such package";
    return result;
  });

  const PackageInstallResult first =
      PackageInstaller::instance().install("failing-test-package");
  const PackageInstallResult second =
      PackageInstaller::instance().install("failing-test-package");

  EXPECT_FALSE(first.ok);
  EXPECT_FALSE(second.ok);
  EXPECT_EQ(calls.load(), 2);  // Not memoized: a failed install may be retried.
}

}  // namespace
}  // namespace agent
