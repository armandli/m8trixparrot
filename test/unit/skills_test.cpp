// SKILL.md frontmatter parsing and directory discovery.

#include <string>

#include <gtest/gtest.h>

#include <core/skills.h>
#include <tool_test_env.h>

namespace agent {
namespace {

TEST(SkillFrontmatterTest, ParsesNameAndDescription) {
  const SkillFrontmatter fm = parse_frontmatter(
      "---\n"
      "name: pdf-tools\n"
      "description: Work with PDFs. Use when the user mentions PDFs.\n"
      "---\n"
      "# body\n");

  EXPECT_TRUE(fm.ok);
  EXPECT_EQ("pdf-tools", fm.name);
  EXPECT_EQ("Work with PDFs. Use when the user mentions PDFs.", fm.description);
}

TEST(SkillFrontmatterTest, FlattensMetadataAndStripsQuotes) {
  const SkillFrontmatter fm = parse_frontmatter(
      "---\n"
      "name: x\n"
      "description: does x\n"
      "license: Apache-2.0\n"
      "metadata:\n"
      "  requires: skill-a skill-b\n"
      "  command: \"true\"\n"
      "  argument-hint: '<n> [format]'\n"
      "version: \"9.9\"\n"  // dedented -> back to top level, ignored
      "---\n");

  EXPECT_TRUE(fm.ok);
  EXPECT_EQ("skill-a skill-b", fm.metadata.at("requires"));
  EXPECT_EQ("true", fm.metadata.at("command"));
  EXPECT_EQ("<n> [format]", fm.metadata.at("argument-hint"));
  EXPECT_EQ(fm.metadata.find("version"), fm.metadata.end());
}

TEST(SkillFrontmatterTest, RejectsMissingFence) {
  EXPECT_FALSE(parse_frontmatter("name: x\ndescription: y\n").ok);
}

TEST(SkillFrontmatterTest, RejectsUnclosedFence) {
  EXPECT_FALSE(parse_frontmatter("---\nname: x\ndescription: y\n").ok);
}

TEST(SkillFrontmatterTest, RejectsEmptyDescription) {
  EXPECT_FALSE(parse_frontmatter("---\nname: x\ndescription:\n---\n").ok);
}

TEST(SkillFrontmatterTest, HandlesCarriageReturns) {
  const SkillFrontmatter fm =
      parse_frontmatter("---\r\nname: x\r\ndescription: crlf ok\r\n---\r\n");
  EXPECT_TRUE(fm.ok);
  EXPECT_EQ("crlf ok", fm.description);
}

struct SkillCatalogTest : test::ToolTest {};

TEST_F(SkillCatalogTest, DiscoversSortsAndSkips) {
  write_file(".m8trix/skills/beta/SKILL.md",
             "---\nname: beta\ndescription: second\n---\n");
  write_file(".m8trix/skills/alpha/SKILL.md",
             "---\nname: alpha\ndescription: first\n---\n");
  write_file(".m8trix/skills/gamma/notes.txt", "no SKILL.md here");
  write_file(".m8trix/skills/delta/SKILL.md",
             "---\nname: delta\n---\n");  // no description

  const SkillCatalog catalog = SkillCatalog::discover(".m8trix/skills");

  ASSERT_EQ(2u, catalog.skills.size());
  EXPECT_EQ("alpha", catalog.skills[0].name);
  EXPECT_EQ("beta", catalog.skills[1].name);
  EXPECT_EQ("first", catalog.skills[0].description);
  EXPECT_TRUE(catalog.find("alpha") != nullptr);
  EXPECT_TRUE(catalog.find("gamma") == nullptr);

  bool noted_delta = false;
  for (const std::string& note : catalog.notes) {
    if (note.find("delta") != std::string::npos) noted_delta = true;
  }
  EXPECT_TRUE(noted_delta);
}

TEST_F(SkillCatalogTest, ParsesCommandMetadata) {
  write_file(
      ".m8trix/skills/deploy/SKILL.md",
      "---\nname: deploy\ndescription: ship it\nmetadata:\n"
      "  command: \"true\"\n  argument-hint: \"[env]\"\n  requires: build test\n"
      "  model-invocable: \"false\"\n---\n");

  const SkillCatalog catalog = SkillCatalog::discover(".m8trix/skills");
  ASSERT_EQ(1u, catalog.skills.size());
  const SkillInfo& s = catalog.skills[0];
  EXPECT_TRUE(s.command);
  EXPECT_EQ("[env]", s.argument_hint);
  EXPECT_EQ((std::vector<std::string>{"build", "test"}), s.dependencies);
  EXPECT_FALSE(s.model_invocable);
}

TEST_F(SkillCatalogTest, NoteOnNameDirMismatchButStillLoads) {
  write_file(".m8trix/skills/real-name/SKILL.md",
             "---\nname: other-name\ndescription: d\n---\n");

  const SkillCatalog catalog = SkillCatalog::discover(".m8trix/skills");
  ASSERT_EQ(1u, catalog.skills.size());
  EXPECT_EQ("real-name", catalog.skills[0].name);  // directory wins
  EXPECT_FALSE(catalog.notes.empty());
}

TEST_F(SkillCatalogTest, MissingDirectoryIsEmpty) {
  EXPECT_TRUE(SkillCatalog::discover(".m8trix/skills").skills.empty());
  EXPECT_TRUE(SkillCatalog::discover("").skills.empty());
}

TEST_F(SkillCatalogTest, LabelForTextMatchesSkillDirectory) {
  write_file(".m8trix/skills/notes/SKILL.md",
             "---\nname: notes\ndescription: d\n---\n");
  const SkillCatalog catalog = SkillCatalog::discover(".m8trix/skills");

  EXPECT_EQ("notes", catalog.label_for_text(
                         "open('.m8trix/skills/notes/references/x.md')"));
  EXPECT_EQ("", catalog.label_for_text("print('hello')"));
}

// The example skill checked into the repo must parse and declare its command.
TEST(BundledSkillsTest, TodoScanIsValid) {
  const SkillCatalog catalog =
      SkillCatalog::discover(std::string(M8_SOURCE_DIR) + "/.m8trix/skills");

  const SkillInfo* todo = catalog.find("todo-scan");
  ASSERT_TRUE(todo != nullptr);
  EXPECT_TRUE(todo->command);
  EXPECT_FALSE(todo->description.empty());
  EXPECT_FALSE(todo->argument_hint.empty());
}

}  // namespace
}  // namespace agent
