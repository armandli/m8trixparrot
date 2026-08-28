// The `skill` tool end to end: a scripted turn loads a skill (its SKILL.md body
// lands in the transcript, tagged) then unloads it (the tagged messages are
// removed). Also: `tool_names()` advertises `skill` only when a catalog exists.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent.h>
#include <core/agent_pool.h>
#include <core/ollama_client.h>
#include <core/policy.h>
#include <core/session_store.h>
#include <loopback_server.h>

namespace agent {
namespace {

constexpr const char* kBodyMarker = "ZZZ_SKILL_BODY_MARKER_ZZZ";

struct AgentSkillsTest : ::testing::Test {
  std::filesystem::path root;
  YoloPolicy policy;

  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("m8trix-skills-" + generate_uuid_v4());
    std::filesystem::create_directories(root / "skills" / "demo");
    std::ofstream(root / "skills" / "demo" / "SKILL.md")
        << "---\nname: demo\ndescription: a demo skill\n---\n\n# demo\n"
        << kBodyMarker << "\n";
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    AgentPool::instance().set_observer({});
  }

  AgentOptions opts() const {
    AgentOptions o;
    o.max_steps = 6;
    o.context_summarize_at_tokens = 100000000;
    o.skills_dir = (root / "skills").string();
    return o;
  }
};

TEST_F(AgentSkillsTest, ToolNamesAdvertiseSkillWhenCatalogExists) {
  const std::string id = AgentPool::instance().register_root("root");
  const Agent agent(opts(), policy, id, "", 0);

  const std::vector<std::string> names = agent.tool_names();
  EXPECT_NE(std::find(names.begin(), names.end(), "skill"), names.end());

  AgentOptions empty = opts();
  empty.skills_dir = (root / "nonexistent").string();
  const Agent bare(empty, policy, id, "", 0);
  const std::vector<std::string> bare_names = bare.tool_names();
  EXPECT_EQ(std::find(bare_names.begin(), bare_names.end(), "skill"),
            bare_names.end());
}

TEST_F(AgentSkillsTest, LoadThenUnloadRemovesSkillFromTranscript) {
  test::LoopbackServer server({
      // step 1: load the skill
      R"json({"message":{"role":"assistant","content":"","tool_calls":[{"function":{"name":"skill","arguments":{"action":"load","name":"demo"}}}]},"done":true,"prompt_eval_count":300,"eval_count":4})json",
      // step 2: unload it
      R"json({"message":{"role":"assistant","content":"","tool_calls":[{"function":{"name":"skill","arguments":{"action":"unload","name":"demo"}}}]},"done":true,"prompt_eval_count":9000,"eval_count":4})json",
      // step 3: final answer
      R"json({"message":{"role":"assistant","content":"all done"},"done":true,"prompt_eval_count":150,"eval_count":3})json",
  });
  OllamaClient::configure("test-model", server.url(""));
  OllamaClient::set_num_ctx(0);

  std::vector<std::string> skill_results;
  AgentPool::instance().set_observer([&](const AgentEvent& e) {
    if (e.kind == AgentEvent::Kind::ToolResult and e.tool_name == "skill") {
      skill_results.push_back(e.text);
    }
  });

  const std::string id = AgentPool::instance().register_root("root");
  Agent agent(opts(), policy, id, "", 0);
  const AgentResult result = agent.run_turn("use the demo skill");

  AgentPool::instance().set_observer({});

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ("all done", result.conclusion);

  ASSERT_EQ(2u, skill_results.size());
  EXPECT_NE(skill_results[0].find(kBodyMarker), std::string::npos)
      << "load should return the SKILL.md body";
  EXPECT_NE(skill_results[1].find("unloaded skill 'demo'"), std::string::npos)
      << skill_results[1];

  for (const ChatMessage& message : agent.transcript()) {
    EXPECT_EQ(message.content.find(kBodyMarker), std::string::npos)
        << "unload should have removed the skill body from the transcript";
    EXPECT_TRUE(message.skill_label.empty());
  }
}

}  // namespace
}  // namespace agent
