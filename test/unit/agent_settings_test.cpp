#include <core/agent_settings.h>

#include <gtest/gtest.h>

#include <tool_test_env.h>

namespace agent {
namespace {

struct AgentSettingsTest : test::ToolTest {};

TEST_F(AgentSettingsTest, MissingFileLeavesEverythingUnsetAndNoWarning) {
  std::string warning;
  const StartupSettings settings = load_startup_settings("settings.json", warning);

  EXPECT_TRUE(warning.empty());
  EXPECT_FALSE(settings.model.has_value());
  EXPECT_FALSE(settings.policy.has_value());
  EXPECT_FALSE(settings.max_steps.has_value());
  EXPECT_FALSE(settings.max_depth.has_value());
  EXPECT_FALSE(settings.max_agents.has_value());
  EXPECT_FALSE(settings.num_ctx.has_value());
  EXPECT_FALSE(settings.summarize_at.has_value());
  EXPECT_FALSE(settings.skills_dir.has_value());
  EXPECT_FALSE(settings.enable_skills.has_value());
  EXPECT_FALSE(settings.enable_subagents.has_value());
  EXPECT_FALSE(settings.enable_package_install.has_value());
  EXPECT_FALSE(settings.enable_web_search.has_value());
}

TEST_F(AgentSettingsTest, AllRecognizedKeysAreParsed) {
  write_file("settings.json", R"json({
    "model": "qwen3:32b",
    "policy": "yolo",
    "max_steps": 20,
    "max_depth": 5,
    "max_agents": 8,
    "num_ctx": 65536,
    "summarize_at": 100000,
    "skills_dir": "custom/skills",
    "enable_skills": false,
    "enable_subagents": false,
    "enable_package_install": false,
    "enable_web_search": true
  })json");

  std::string warning;
  const StartupSettings settings = load_startup_settings("settings.json", warning);

  EXPECT_TRUE(warning.empty());
  EXPECT_EQ(settings.model, "qwen3:32b");
  EXPECT_EQ(settings.policy, "yolo");
  EXPECT_EQ(settings.max_steps, 20);
  EXPECT_EQ(settings.max_depth, 5);
  EXPECT_EQ(settings.max_agents, 8);
  EXPECT_EQ(settings.num_ctx, 65536);
  EXPECT_EQ(settings.summarize_at, 100000);
  EXPECT_EQ(settings.skills_dir, "custom/skills");
  EXPECT_EQ(settings.enable_skills, false);
  EXPECT_EQ(settings.enable_subagents, false);
  EXPECT_EQ(settings.enable_package_install, false);
  EXPECT_EQ(settings.enable_web_search, true);
}

TEST_F(AgentSettingsTest, PartialFileLeavesTheRestUnset) {
  write_file("settings.json", R"json({"max_steps": 30})json");

  std::string warning;
  const StartupSettings settings = load_startup_settings("settings.json", warning);

  EXPECT_TRUE(warning.empty());
  EXPECT_EQ(settings.max_steps, 30);
  EXPECT_FALSE(settings.model.has_value());
  EXPECT_FALSE(settings.enable_skills.has_value());
}

TEST_F(AgentSettingsTest, WrongTypeForAKeyLeavesItUnsetRatherThanErroring) {
  write_file("settings.json", R"json({"max_steps": "not a number", "model": "ok-model"})json");

  std::string warning;
  const StartupSettings settings = load_startup_settings("settings.json", warning);

  EXPECT_TRUE(warning.empty());
  EXPECT_FALSE(settings.max_steps.has_value());
  EXPECT_EQ(settings.model, "ok-model");
}

TEST_F(AgentSettingsTest, InvalidJsonWarnsAndLeavesEverythingUnset) {
  write_file("settings.json", "{not valid json");

  std::string warning;
  const StartupSettings settings = load_startup_settings("settings.json", warning);

  EXPECT_FALSE(warning.empty());
  EXPECT_FALSE(settings.max_steps.has_value());
}

TEST_F(AgentSettingsTest, NonObjectTopLevelWarnsAndLeavesEverythingUnset) {
  write_file("settings.json", "[1, 2, 3]");

  std::string warning;
  const StartupSettings settings = load_startup_settings("settings.json", warning);

  EXPECT_FALSE(warning.empty());
  EXPECT_FALSE(settings.max_steps.has_value());
}

// ---------------------------------------------------------------------------
// load_shellrc_settings — m8trixsh's ~/.m8shrc, a shell-env-style file.
// ---------------------------------------------------------------------------

struct ShellRcTest : test::ToolTest {};

TEST_F(ShellRcTest, MissingFileLeavesEverythingUnsetAndNoWarning) {
  std::string warning;
  const StartupSettings settings = load_shellrc_settings(".m8shrc", warning);

  EXPECT_TRUE(warning.empty());
  EXPECT_FALSE(settings.model.has_value());
  EXPECT_FALSE(settings.enable_web_search.has_value());
}

TEST_F(ShellRcTest, KeyValueLinesArePickedUpByTheirUppercaseNames) {
  write_file(".m8shrc",
             "MODEL=qwen3:32b\n"
             "POLICY=sane\n"
             "MAX_STEPS=25\n"
             "MODE_SWITCH_KEY=ctrl-o\n");

  std::string warning;
  const StartupSettings settings = load_shellrc_settings(".m8shrc", warning);

  EXPECT_TRUE(warning.empty());
  EXPECT_EQ(settings.model, "qwen3:32b");
  EXPECT_EQ(settings.policy, "sane");
  EXPECT_EQ(settings.max_steps, 25);
  EXPECT_EQ(settings.mode_switch_key, "ctrl-o");
}

TEST_F(ShellRcTest, TruthyEnableValuesAreTrueAndEverythingElseIsFalse) {
  write_file(".m8shrc",
             "ENABLE_WEB_SEARCH=1\n"
             "ENABLE_SKILLS=on\n"
             "ENABLE_SUBAGENTS=YES\n"
             "ENABLE_PACKAGE_INSTALL=false\n");

  std::string warning;
  const StartupSettings settings = load_shellrc_settings(".m8shrc", warning);

  EXPECT_EQ(settings.enable_web_search, true);
  EXPECT_EQ(settings.enable_skills, true);
  EXPECT_EQ(settings.enable_subagents, true);
  EXPECT_EQ(settings.enable_package_install, false);
}

TEST_F(ShellRcTest, CommentsBlankLinesAndAnExportPrefixAreAllTolerated) {
  write_file(".m8shrc",
             "# a full-line comment\n"
             "\n"
             "  export MODEL=llama3   # trailing comment\n"
             "POLICY=yolo\n");

  std::string warning;
  const StartupSettings settings = load_shellrc_settings(".m8shrc", warning);

  EXPECT_TRUE(warning.empty());
  EXPECT_EQ(settings.model, "llama3");
  EXPECT_EQ(settings.policy, "yolo");
}

TEST_F(ShellRcTest, SurroundingQuotesAreStrippedAndProtectAHashFromComments) {
  write_file(".m8shrc",
             "SHELL=\"/bin/zsh\"\n"
             "MODE_SWITCH_KEY='f12'\n"
             "MODEL=\"weird#model\"\n");

  std::string warning;
  const StartupSettings settings = load_shellrc_settings(".m8shrc", warning);

  EXPECT_EQ(settings.shell, "/bin/zsh");
  EXPECT_EQ(settings.mode_switch_key, "f12");
  EXPECT_EQ(settings.model, "weird#model");
}

TEST_F(ShellRcTest, AnUnparseableIntIsLeftUnset) {
  write_file(".m8shrc", "MAX_STEPS=lots\nNUM_CTX=  \n");

  std::string warning;
  const StartupSettings settings = load_shellrc_settings(".m8shrc", warning);

  EXPECT_FALSE(settings.max_steps.has_value());
  EXPECT_FALSE(settings.num_ctx.has_value());
}

TEST_F(ShellRcTest, LinesWithoutAnEqualsAreIgnored) {
  write_file(".m8shrc", "this is not a setting\nMODEL=ok\n");

  std::string warning;
  const StartupSettings settings = load_shellrc_settings(".m8shrc", warning);

  EXPECT_TRUE(warning.empty());
  EXPECT_EQ(settings.model, "ok");
}

TEST_F(ShellRcTest, PromptKeysAreParsed) {
  write_file(".m8shrc",
             "PROMPT_FORMAT=%tag %F{cyan}%~%f %git\n"
             "PROMPT_SHELL_TAG=[sh]\n"
             "PROMPT_AI_TAG=[ai]\n"
             "PROMPT_ASK_TAG=[ai?]\n");

  std::string warning;
  const StartupSettings settings = load_shellrc_settings(".m8shrc", warning);

  EXPECT_TRUE(warning.empty());
  EXPECT_EQ(settings.prompt_format, "%tag %F{cyan}%~%f %git");
  EXPECT_EQ(settings.prompt_shell_tag, "[sh]");
  EXPECT_EQ(settings.prompt_ai_tag, "[ai]");
  EXPECT_EQ(settings.prompt_ask_tag, "[ai?]");
}

TEST_F(ShellRcTest, AnUnknownKeyIsIgnoredButNamedInTheWarning) {
  write_file(".m8shrc", "ENABLE_WEBSERCH=1\nMODEL=ok\n");

  std::string warning;
  const StartupSettings settings = load_shellrc_settings(".m8shrc", warning);

  EXPECT_EQ(settings.model, "ok");
  EXPECT_FALSE(settings.enable_web_search.has_value());
  EXPECT_NE(warning.find("ENABLE_WEBSERCH"), std::string::npos);
}

}  // namespace
}  // namespace agent
