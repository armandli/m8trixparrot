// PackageInstallTool's own contract: argument validation and how it words a
// PackageInstaller result for the model. The dedup/queueing behavior itself is
// covered by package_installer_test.cpp; this just checks the tool wraps it
// correctly. Threaded (via the PackageInstaller singleton's worker thread), so
// it lives in core_tests alongside package_installer_test.cpp and
// ollama_client_test.cpp rather than in the chdir-based unit_tests binary.

#include <string>

#include <gtest/gtest.h>

#include <core/package_installer.h>
#include <core/tools.h>
#include <tool_test_env.h>

namespace agent::test {
namespace {

TEST(PackageInstallToolTest, MissingPackageIsAnError) {
  const ToolResult result = PackageInstallTool().execute(args({}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "package_install: missing required string argument 'package'");
}

TEST(PackageInstallToolTest, EmptyPackageIsAnError) {
  const ToolResult result = PackageInstallTool().execute(args({{"package", str("")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "package_install: missing required string argument 'package'");
}

TEST(PackageInstallToolTest, DescriptionNamesTheSingleRequiredArgument) {
  const std::string description = PackageInstallTool().description();

  EXPECT_NE(description.find("\"name\":\"package_install\""), std::string::npos);
  EXPECT_NE(description.find("\"package\""), std::string::npos);
  EXPECT_NE(description.find("\"required\":[\"package\"]"), std::string::npos);
}

TEST(PackageInstallToolTest, SuccessfulInstallReportsInstalled) {
  PackageInstaller::set_runner_for_test([](const std::string& package) {
    PackageInstallResult result;
    result.ok = true;
    result.output = "Successfully installed " + package;
    return result;
  });

  const ToolResult result =
      PackageInstallTool().execute(args({{"package", str("tool-test-package-ok")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_NE(result.output.find("installed 'tool-test-package-ok'"), std::string::npos);
  EXPECT_TRUE(result.error.empty());
}

TEST(PackageInstallToolTest, AlreadyInstalledIsWordedDifferentlyAndOk) {
  PackageInstaller::set_runner_for_test([](const std::string&) {
    PackageInstallResult result;
    result.ok = true;
    result.already_installed = true;
    result.output = "already there";
    return result;
  });

  const ToolResult result =
      PackageInstallTool().execute(args({{"package", str("tool-test-package-already")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_NE(result.output.find("already installed"), std::string::npos);
}

TEST(PackageInstallToolTest, FailedInstallIsAnError) {
  PackageInstaller::set_runner_for_test([](const std::string&) {
    PackageInstallResult result;
    result.ok = false;
    result.error = "no matching distribution";
    return result;
  });

  const ToolResult result =
      PackageInstallTool().execute(args({{"package", str("tool-test-package-fail")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("no matching distribution"), std::string::npos);
}

}  // namespace
}  // namespace agent::test
