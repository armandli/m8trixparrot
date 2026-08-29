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
    "enable_package_install": false
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

}  // namespace
}  // namespace agent
