// What `read`, `write` and `edit` do, and where their edges are.
//
// The three of them together are how an agent changes a codebase, so the
// properties that matter most are the ones about *not* changing it by
// accident: edit refuses ambiguous matches and is all-or-nothing, and read
// refuses content it would only garble.

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include <core/tools.h>
#include <tool_test_env.h>

namespace agent::test {
namespace {

struct ReadTest : ToolTest {};
struct WriteTest : ToolTest {};
struct EditTest : ToolTest {};

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------

TEST_F(ReadTest, ReturnsTheWholeFileByDefault) {
  write_file("notes.txt", "alpha\nbeta\ngamma\n");

  const ToolResult result = ReadTool().execute(args({{"path", str("notes.txt")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "alpha\nbeta\ngamma\n");
}

// offset counts from 1, not 0, and limit counts lines rather than bytes.
TEST_F(ReadTest, OffsetIsOneIndexedAndLimitCountsLines) {
  write_file("notes.txt", "one\ntwo\nthree\nfour\nfive\n");

  const ToolResult result = ReadTool().execute(
      args({{"path", str("notes.txt")}, {"offset", num(2)}, {"limit", num(2)}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "two\nthree\n");
}

TEST_F(ReadTest, AnOffsetPastTheEndYieldsEmptyOutputRatherThanAnError) {
  write_file("notes.txt", "one\ntwo\n");

  const ToolResult result = ReadTool().execute(
      args({{"path", str("notes.txt")}, {"offset", num(99)}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "");
}

// Limitation: the default path returns the file's bytes verbatim, but any
// explicit slice is rebuilt line by line with '\n' appended to each — so a
// file with no trailing newline gains one as soon as offset or limit is used.
TEST_F(ReadTest, AnExplicitSliceAppendsATrailingNewlineTheFileMayNotHaveHad) {
  write_file("no_newline.txt", "only line");

  const ToolResult whole =
      ReadTool().execute(args({{"path", str("no_newline.txt")}}));
  EXPECT_EQ(whole.output, "only line");

  const ToolResult sliced = ReadTool().execute(
      args({{"path", str("no_newline.txt")}, {"limit", num(1)}}));
  EXPECT_EQ(sliced.output, "only line\n");
}

// Limitation: the image branch is chosen by file extension alone. The bytes
// are never inspected, so a text file named .png comes back base64-encoded and
// labeled as an image.
TEST_F(ReadTest, ImageDispatchIsByExtensionAndNeverLooksAtTheBytes) {
  write_file("not_really.png", "this is plain text");

  const ToolResult result =
      ReadTool().execute(args({{"path", str("not_really.png")}}));

  EXPECT_TRUE(result.ok);
  // "this is plain text" base64-encoded.
  EXPECT_EQ(result.output, "[image/png; base64]\ndGhpcyBpcyBwbGFpbiB0ZXh0");
}

TEST_F(ReadTest, RecognizedImageExtensionsAreCaseInsensitive) {
  write_file("photo.JPG", "x");

  const ToolResult result = ReadTool().execute(args({{"path", str("photo.JPG")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_NE(result.output.find("[image/jpeg; base64]"), std::string::npos);
}

TEST_F(ReadTest, AFileWithANulByteIsRefusedAsBinary) {
  write_file("blob.dat", std::string("text\0more", 9));

  const ToolResult result = ReadTool().execute(args({{"path", str("blob.dat")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "read: blob.dat looks like a binary file");
}

// Limitation: only the first 8KB is sniffed. A file whose NUL byte arrives
// later reads as text, and those bytes end up in the model's context.
TEST_F(ReadTest, ANulByteAfterTheFirstEightKilobytesIsNotDetected) {
  std::string contents(9000, 'a');
  contents += '\0';
  write_file("late_nul.txt", contents);

  const ToolResult result =
      ReadTool().execute(args({{"path", str("late_nul.txt")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output.size(), contents.size());
}

TEST_F(ReadTest, MissingFileDirectoryAndMissingPathHaveDistinctErrors) {
  std::filesystem::create_directory(dir() / "subdir");

  const ToolResult absent =
      ReadTool().execute(args({{"path", str("nope.txt")}}));
  EXPECT_FALSE(absent.ok);
  EXPECT_EQ(absent.error, "read: no such file: nope.txt");

  const ToolResult directory =
      ReadTool().execute(args({{"path", str("subdir")}}));
  EXPECT_FALSE(directory.ok);
  EXPECT_EQ(directory.error, "read: path is a directory: subdir");

  const ToolResult missing = ReadTool().execute(args({}));
  EXPECT_FALSE(missing.ok);
  EXPECT_EQ(missing.error, "read: missing required string argument 'path'");
}

// ---------------------------------------------------------------------------
// write
// ---------------------------------------------------------------------------

TEST_F(WriteTest, CreatesAFileAndReportsTheByteCount) {
  const ToolResult result = WriteTool().execute(
      args({{"path", str("out.txt")}, {"content", str("hello")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "wrote 5 bytes to out.txt");
  EXPECT_EQ(read_back("out.txt"), "hello");
}

TEST_F(WriteTest, CreatesMissingParentDirectories) {
  const ToolResult result = WriteTool().execute(
      args({{"path", str("a/b/c/deep.txt")}, {"content", str("x")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(read_back("a/b/c/deep.txt"), "x");
}

// Overwrite is wholesale — there is no append mode, and no merge with what was
// there before.
TEST_F(WriteTest, OverwritesAnExistingFileEntirely) {
  write_file("out.txt", "a much longer original body");

  const ToolResult result = WriteTool().execute(
      args({{"path", str("out.txt")}, {"content", str("short")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(read_back("out.txt"), "short");
}

// Empty content is a legitimate write (truncate the file), distinct from
// omitting the argument.
TEST_F(WriteTest, EmptyContentTruncatesTheFileAndIsNotAnError) {
  write_file("out.txt", "something");

  const ToolResult result = WriteTool().execute(
      args({{"path", str("out.txt")}, {"content", str("")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "wrote 0 bytes to out.txt");
  EXPECT_EQ(read_back("out.txt"), "");
}

TEST_F(WriteTest, MissingPathOrContentIsAnError) {
  const ToolResult no_path =
      WriteTool().execute(args({{"content", str("x")}}));
  EXPECT_FALSE(no_path.ok);
  EXPECT_EQ(no_path.error, "write: missing required string argument 'path'");

  const ToolResult no_content =
      WriteTool().execute(args({{"path", str("out.txt")}}));
  EXPECT_FALSE(no_content.ok);
  EXPECT_EQ(no_content.error, "write: missing required string argument 'content'");
  EXPECT_FALSE(exists("out.txt"));
}

// ---------------------------------------------------------------------------
// edit
// ---------------------------------------------------------------------------

TEST_F(EditTest, ReplacesAUniqueStretchOfText) {
  write_file("code.txt", "alpha\nbeta\ngamma\n");

  const ToolResult result = EditTool().execute(
      args({{"path", str("code.txt")}, {"edits", edits({{"beta", "BETA"}})}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output, "applied 1 edit(s) to code.txt");
  EXPECT_EQ(read_back("code.txt"), "alpha\nBETA\ngamma\n");
}

// Edits are placed against the *original* text and applied in position order,
// so the order they appear in the array doesn't matter.
TEST_F(EditTest, SeveralEditsApplyInPositionOrderRegardlessOfArrayOrder) {
  write_file("code.txt", "one\ntwo\nthree\n");

  const ToolResult result = EditTool().execute(
      args({{"path", str("code.txt")},
            {"edits", edits({{"three", "3"}, {"one", "1"}})}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(read_back("code.txt"), "1\ntwo\n3\n");
}

TEST_F(EditTest, AnEmptyNewTextDeletesTheMatchedText) {
  write_file("code.txt", "keep\nDROP\nkeep\n");

  const ToolResult result = EditTool().execute(
      args({{"path", str("code.txt")}, {"edits", edits({{"DROP\n", ""}})}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(read_back("code.txt"), "keep\nkeep\n");
}

// The uniqueness requirement is what makes an edit unambiguous. A string that
// appears twice is refused rather than the tool guessing which one was meant.
TEST_F(EditTest, TextThatAppearsTwiceIsRefusedAsNotUnique) {
  write_file("code.txt", "x = 1\ny = 1\n");

  const ToolResult result = EditTool().execute(
      args({{"path", str("code.txt")}, {"edits", edits({{"1", "2"}})}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "edit: edits[0].oldText is not unique in the file");
}

TEST_F(EditTest, TextThatIsNotPresentIsRefused) {
  write_file("code.txt", "alpha\n");

  const ToolResult result = EditTool().execute(
      args({{"path", str("code.txt")}, {"edits", edits({{"omega", "x"}})}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "edit: edits[0].oldText was not found in the file");
}

TEST_F(EditTest, AnEmptyOldTextIsRefused) {
  write_file("code.txt", "alpha\n");

  const ToolResult result = EditTool().execute(
      args({{"path", str("code.txt")}, {"edits", edits({{"", "x"}})}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "edit: edits[0].oldText is empty");
}

TEST_F(EditTest, EditsThatOverlapEachOtherAreRefused) {
  write_file("code.txt", "abcdef\n");

  const ToolResult result =
      EditTool().execute(args({{"path", str("code.txt")},
                               {"edits", edits({{"abcd", "x"}, {"cdef", "y"}})}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "edit: edits overlap each other; merge them into one edit");
}

// All-or-nothing: every edit is placed before any is applied, so a bad edit
// anywhere in the list leaves the file byte-identical. This is the property
// that makes a failed edit safe to retry.
TEST_F(EditTest, AFailedEditLeavesTheFileCompletelyUntouched) {
  const std::string original = "alpha\nbeta\ngamma\n";
  write_file("code.txt", original);

  const ToolResult result = EditTool().execute(
      args({{"path", str("code.txt")},
            {"edits", edits({{"alpha", "ALPHA"}, {"nowhere", "x"}})}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(read_back("code.txt"), original);
}

// Limitation: matching is on raw bytes. No regex, no whitespace tolerance —
// differing indentation means no match.
TEST_F(EditTest, MatchingIsRawBytesWithNoRegexOrWhitespaceTolerance) {
  write_file("code.txt", "  indented line\n");

  const ToolResult regex_attempt = EditTool().execute(
      args({{"path", str("code.txt")}, {"edits", edits({{".*line", "x"}})}}));
  EXPECT_FALSE(regex_attempt.ok);

  const ToolResult whitespace_attempt = EditTool().execute(
      args({{"path", str("code.txt")}, {"edits", edits({{"indented line", "x"}})}}));
  EXPECT_TRUE(whitespace_attempt.ok);
  EXPECT_EQ(read_back("code.txt"), "  x\n");
}

TEST_F(EditTest, MissingFileOrMissingEditsIsAnError) {
  const ToolResult absent = EditTool().execute(
      args({{"path", str("nope.txt")}, {"edits", edits({{"a", "b"}})}}));
  EXPECT_FALSE(absent.ok);
  EXPECT_EQ(absent.error, "edit: failed to read nope.txt");

  write_file("code.txt", "alpha\n");
  const ToolResult no_edits = EditTool().execute(args({{"path", str("code.txt")}}));
  EXPECT_FALSE(no_edits.ok);
  EXPECT_EQ(no_edits.error,
            "edit: missing required argument 'edits' (array of {oldText, newText})");
}

}  // namespace
}  // namespace agent::test
