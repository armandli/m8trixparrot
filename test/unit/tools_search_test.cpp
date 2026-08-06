// What `find` and `grep` match, and what they quietly skip.
//
// Both walk the tree relative to a search root and both consult .gitignore.
// The recurring trap in each is the same: patterns are matched against paths
// *relative to the root*, so a pattern that looks like it should match a
// filename anywhere only matches at the top level.

#include <string>

#include <gtest/gtest.h>

#include <core/tools.h>
#include <tool_test_env.h>

namespace agent::test {
namespace {

struct FindTest : ToolTest {};
struct GrepTest : ToolTest {};

// A small tree used by most of the find tests.
void build_tree(const ToolTest& env) {
  env.write_file("top.cpp", "");
  env.write_file("top.txt", "");
  env.write_file("src/inner.cpp", "");
  env.write_file("src/deep/deeper.cpp", "");
}

// ---------------------------------------------------------------------------
// find
// ---------------------------------------------------------------------------

// THE trap. `*.cpp` matches only files sitting directly in the search dir,
// because the pattern is matched against the relative path and `*` does not
// cross a '/'. `**/*.cpp` is what "anywhere below here" looks like.
TEST_F(FindTest, PatternsMatchRelativePathsSoStarDoesNotCrossDirectories) {
  build_tree(*this);

  const ToolResult shallow = FindTool().execute(args({{"pattern", str("*.cpp")}}));
  EXPECT_TRUE(shallow.ok);
  EXPECT_EQ(shallow.output, "top.cpp\n");

  const ToolResult recursive =
      FindTool().execute(args({{"pattern", str("**/*.cpp")}}));
  EXPECT_TRUE(recursive.ok);
  EXPECT_EQ(recursive.output, "src/deep/deeper.cpp\nsrc/inner.cpp\ntop.cpp\n");
}

// `**/` stands for zero or more leading directories, which is why the
// recursive pattern above also matched the top-level file.
TEST_F(FindTest, DoubleStarSlashMatchesZeroLeadingDirectories) {
  build_tree(*this);

  const ToolResult result =
      FindTool().execute(args({{"pattern", str("**/top.cpp")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "top.cpp\n");
}

TEST_F(FindTest, QuestionMarkMatchesOneCharacterWithinASegment) {
  write_file("a1.txt", "");
  write_file("a12.txt", "");

  const ToolResult result = FindTool().execute(args({{"pattern", str("a?.txt")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "a1.txt\n");
}

TEST_F(FindTest, ResultsAreSortedAndPathsAreRelativeToTheSearchDir) {
  build_tree(*this);

  const ToolResult result = FindTool().execute(
      args({{"pattern", str("**/*.cpp")}, {"path", str("src")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "deep/deeper.cpp\ninner.cpp\n");
}

// Inside a repository, .gitignore is honored and ignored directories are never
// descended into. .git itself is always skipped.
TEST_F(FindTest, GitignoredPathsAreSkippedInsideARepository) {
  init_git_repo();
  write_file(".gitignore", "build/\n*.log\n");
  write_file("keep.cpp", "");
  write_file("noisy.log", "");
  write_file("build/generated.cpp", "");

  const ToolResult result = FindTool().execute(args({{"pattern", str("**/*")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_NE(result.output.find("keep.cpp"), std::string::npos);
  EXPECT_EQ(result.output.find("noisy.log"), std::string::npos);
  EXPECT_EQ(result.output.find("build/generated.cpp"), std::string::npos);
  EXPECT_EQ(result.output.find(".git/"), std::string::npos);
}

// Outside a repository there is nothing to consult, so nothing is ignored —
// a .gitignore file sitting in a plain directory has no effect at all.
TEST_F(FindTest, OutsideARepositoryAGitignoreFileHasNoEffect) {
  write_file(".gitignore", "*.log\n");
  write_file("noisy.log", "");

  const ToolResult result = FindTool().execute(args({{"pattern", str("**/*.log")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "noisy.log\n");
}

TEST_F(FindTest, ExceedingTheLimitCapsResultsAndFlagsTruncation) {
  for (int i = 0; i < 5; ++i) {
    write_file("file" + std::to_string(i) + ".txt", "");
  }

  const ToolResult result = FindTool().execute(
      args({{"pattern", str("*.txt")}, {"limit", num(2)}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.truncated);
  EXPECT_NE(result.output.find("[results capped at 2]"), std::string::npos);
  EXPECT_NE(result.output.find("file0.txt"), std::string::npos);
  EXPECT_EQ(result.output.find("file4.txt"), std::string::npos);
}

// Limitation: the schema advertises a "bad glob pattern" error, but
// glob_to_regex() escapes every regex metacharacter on the way through, so a
// pattern that would be a broken regex is simply treated as literal text. The
// error is effectively unreachable.
TEST_F(FindTest, AGlobThatWouldBeABrokenRegexIsTreatedAsLiteralText) {
  write_file("[unclosed", "");

  const ToolResult result =
      FindTool().execute(args({{"pattern", str("[unclosed")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "[unclosed\n");
}

TEST_F(FindTest, NoMatchesYieldsEmptyOutputRatherThanAnError) {
  build_tree(*this);

  const ToolResult result = FindTool().execute(args({{"pattern", str("*.rs")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "");
}

TEST_F(FindTest, MissingPatternIsAnError) {
  const ToolResult result = FindTool().execute(args({}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "find: missing required string argument 'pattern'");
}

// ---------------------------------------------------------------------------
// grep
// ---------------------------------------------------------------------------

TEST_F(GrepTest, MatchedLinesUseAColonAndCarryFileAndLineNumber) {
  write_file("a.txt", "first\nneedle here\nlast\n");

  const ToolResult result = GrepTool().execute(args({{"pattern", str("needle")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "a.txt:2:needle here\n");
}

TEST_F(GrepTest, ThePatternIsARegexUnlessLiteralIsSet) {
  write_file("a.txt", "value = 42\nliteral a.c here\n");

  const ToolResult as_regex =
      GrepTool().execute(args({{"pattern", str("[0-9]+")}}));
  EXPECT_EQ(as_regex.output, "a.txt:1:value = 42\n");

  // As a regex, "a.c" matches "a c", "abc", ... ; as a literal it matches only
  // the three characters.
  const ToolResult as_literal = GrepTool().execute(
      args({{"pattern", str("a.c")}, {"literal", flag(true)}}));
  EXPECT_EQ(as_literal.output, "a.txt:2:literal a.c here\n");
}

TEST_F(GrepTest, IgnoreCaseAppliesToBothRegexAndLiteralMatching) {
  write_file("a.txt", "MixedCase\n");

  const ToolResult regex_mode = GrepTool().execute(
      args({{"pattern", str("mixedcase")}, {"ignoreCase", flag(true)}}));
  EXPECT_EQ(regex_mode.output, "a.txt:1:MixedCase\n");

  const ToolResult literal_mode =
      GrepTool().execute(args({{"pattern", str("mixedcase")},
                               {"literal", flag(true)},
                               {"ignoreCase", flag(true)}}));
  EXPECT_EQ(literal_mode.output, "a.txt:1:MixedCase\n");
}

// Context lines are marked with '-' where matches use ':', so a reader (and a
// model) can tell which line actually matched.
TEST_F(GrepTest, ContextLinesAreMarkedWithADashInsteadOfAColon) {
  write_file("a.txt", "one\ntwo\nneedle\nfour\nfive\n");

  const ToolResult result = GrepTool().execute(
      args({{"pattern", str("needle")}, {"context", num(1)}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "a.txt-2-two\na.txt:3:needle\na.txt-4-four\n");
}

TEST_F(GrepTest, PathMayNameASingleFileInsteadOfADirectory) {
  write_file("a.txt", "needle\n");
  write_file("b.txt", "needle\n");

  const ToolResult result = GrepTool().execute(
      args({{"pattern", str("needle")}, {"path", str("a.txt")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "a.txt:1:needle\n");
}

// The glob filter uses the same relative-path rule as find, with the same
// consequence: `*.txt` reaches only the top level.
TEST_F(GrepTest, TheGlobFilterUsesTheSameRelativePathRuleAsFind) {
  write_file("top.txt", "needle\n");
  write_file("sub/inner.txt", "needle\n");
  write_file("sub/inner.md", "needle\n");

  const ToolResult shallow = GrepTool().execute(
      args({{"pattern", str("needle")}, {"glob", str("*.txt")}}));
  EXPECT_EQ(shallow.output, "top.txt:1:needle\n");

  const ToolResult recursive = GrepTool().execute(
      args({{"pattern", str("needle")}, {"glob", str("**/*.txt")}}));
  EXPECT_NE(recursive.output.find("sub/inner.txt:1:needle"), std::string::npos);
  EXPECT_EQ(recursive.output.find("inner.md"), std::string::npos);
}

TEST_F(GrepTest, NoMatchesIsReportedAsNoMatchesRatherThanEmptyOutput) {
  write_file("a.txt", "nothing here\n");

  const ToolResult result = GrepTool().execute(args({{"pattern", str("needle")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "[no matches]");
}

TEST_F(GrepTest, ReachingTheMatchLimitStopsTheSearchAndSaysSo) {
  write_file("a.txt", "needle\nneedle\nneedle\nneedle\n");

  const ToolResult result = GrepTool().execute(
      args({{"pattern", str("needle")}, {"limit", num(2)}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.truncated);
  EXPECT_NE(result.output.find("[stopped at 2 matches]"), std::string::npos);
  EXPECT_EQ(result.output.find("a.txt:3:"), std::string::npos);
}

// A CRLF file matches as if it were LF: the trailing '\r' is stripped before
// the pattern is applied, so an anchored pattern still works.
TEST_F(GrepTest, ACarriageReturnAtEndOfLineIsStrippedBeforeMatching) {
  write_file("crlf.txt", "needle\r\n");

  const ToolResult result =
      GrepTool().execute(args({{"pattern", str("needle$")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "crlf.txt:1:needle\n");
}

// Limitation: matching is line by line, so a pattern spanning a newline can
// never match no matter how it is written.
TEST_F(GrepTest, APatternSpanningANewlineNeverMatches) {
  write_file("a.txt", "alpha\nbeta\n");

  const ToolResult result =
      GrepTool().execute(args({{"pattern", str("alpha\\nbeta")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "[no matches]");
}

// Limitation: binary detection looks at the first line only. A file whose NUL
// byte appears later is searched like text, and its bytes reach the output.
TEST_F(GrepTest, BinaryDetectionOnlyInspectsTheFirstLine) {
  write_file("early.bin", std::string("h\0dr\nneedle\n", 12));
  write_file("late.bin", std::string("header\nneedle\0here\n", 19));

  const ToolResult result = GrepTool().execute(args({{"pattern", str("needle")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output.find("early.bin"), std::string::npos);
  EXPECT_NE(result.output.find("late.bin:2:"), std::string::npos);
}

TEST_F(GrepTest, AnInvalidRegexIsReportedAsAnError) {
  write_file("a.txt", "x\n");

  const ToolResult result = GrepTool().execute(args({{"pattern", str("a(b")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error.rfind("grep: bad regex pattern: ", 0), 0u);
}

TEST_F(GrepTest, MissingPatternIsAnError) {
  const ToolResult result = GrepTool().execute(args({}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "grep: missing required string argument 'pattern'");
}

}  // namespace
}  // namespace agent::test
