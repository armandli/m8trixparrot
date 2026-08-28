// What PythonTool does, and its key invariants.
//
// Like BashTool, ok == true means the script *ran*. Python exceptions and
// stderr output are content returned to the model, not tool failures. Only a
// missing argument or an interpreter-level crash is an error.

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include <core/tools.h>
#include <tool_test_env.h>

namespace agent::test {
namespace {

struct PythonTest : ToolTest {};

TEST_F(PythonTest, CapturesStdout) {
    const ToolResult result = PythonTool().execute(args({{"script", str("print('hello')")}}));

    EXPECT_TRUE(result.ok);
    EXPECT_NE(result.output.find("hello"), std::string::npos);
    EXPECT_TRUE(result.error.empty());
    EXPECT_FALSE(result.truncated);
}

TEST_F(PythonTest, CapturesStderr) {
    const ToolResult result = PythonTool().execute(
        args({{"script", str("import sys; sys.stderr.write('oops')")}}));

    EXPECT_TRUE(result.ok);
    EXPECT_NE(result.output.find("oops"), std::string::npos);
    EXPECT_TRUE(result.error.empty());
}

// The headline contract: a Python exception doesn't make ok == false.
// The traceback is part of the output so the model can read and react to it.
TEST_F(PythonTest, ExceptionIsOkAndAppearsInOutput) {
    const ToolResult result = PythonTool().execute(
        args({{"script", str("raise ValueError('bad input')")}}));

    EXPECT_TRUE(result.ok);
    EXPECT_NE(result.output.find("ValueError"), std::string::npos);
    EXPECT_NE(result.output.find("bad input"), std::string::npos);
    EXPECT_TRUE(result.error.empty());
}

TEST_F(PythonTest, StdlibIsAccessible) {
    const ToolResult result = PythonTool().execute(
        args({{"script", str("import math; print(math.floor(math.pi))")}}));

    EXPECT_TRUE(result.ok);
    EXPECT_NE(result.output.find("3"), std::string::npos);
}

TEST_F(PythonTest, MultiLineScriptRuns) {
    const ToolResult result = PythonTool().execute(args({{"script", str(R"(
x = 6
y = 7
print(x * y)
)")}}));

    EXPECT_TRUE(result.ok);
    EXPECT_NE(result.output.find("42"), std::string::npos);
}

TEST_F(PythonTest, MissingScriptIsAnError) {
    const ToolResult result = PythonTool().execute(args({}));

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "python: missing required string argument 'script'");
    EXPECT_TRUE(result.output.empty());
}

TEST_F(PythonTest, EmptyScriptIsAnError) {
    const ToolResult result = PythonTool().execute(args({{"script", str("")}}));

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "python: missing required string argument 'script'");
}

TEST_F(PythonTest, WrongArgTypeReadsAsMissing) {
    const ToolResult result = PythonTool().execute(args({{"script", num(42)}}));

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "python: missing required string argument 'script'");
}

// stdout and stderr are restored after the script runs, so a second call is
// not affected by the first script's redirections.
TEST_F(PythonTest, ConsecutiveCallsAreIsolated) {
    PythonTool().execute(args({{"script", str("print('first')")}}));

    const ToolResult result = PythonTool().execute(args({{"script", str("print('second')")}}));

    EXPECT_TRUE(result.ok);
    EXPECT_NE(result.output.find("second"), std::string::npos);
    EXPECT_EQ(result.output.find("first"), std::string::npos);
}

// Package installation is not a capability the tool offers, so the schema must
// not tell the model to run pip.
TEST(PythonToolTest, DescriptionDoesNotAdvertisePackageInstall) {
    const std::string description = PythonTool().description();

    EXPECT_EQ(description.find("pip"), std::string::npos) << description;
    EXPECT_EQ(description.find("subprocess"), std::string::npos) << description;
}

// ensure_python_ready() denies pip a package index, so a `pip install` a script
// shells out to anyway fails fast instead of writing to the environment.
TEST(PythonToolTest, PackageIndexIsDisabled) {
    ensure_python_ready();

    const char* no_index = std::getenv("PIP_NO_INDEX");
    ASSERT_NE(no_index, nullptr);
    EXPECT_STREQ(no_index, "1");
}

}  // namespace
}  // namespace agent::test
