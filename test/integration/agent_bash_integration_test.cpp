// Live end-to-end tests. A real Agent turn, a real Ollama model, and m8's own
// tool set: `bash_repl` as the sole execution substrate, `bash_search` for
// finding commands, and the installed tool_* binaries reached by running them in
// the shell. Each test hands the agent a plain-English objective and checks the
// side effect it should have produced — a file on disk, a value in the final
// message — never the exact wording, since the model is non-deterministic.
//
// Skipped unless Ollama is reachable with the model (default "qwen3.8:27b-mlx",
// matching src/apps/m8/main.cpp; override with OLLAMA_HOST / M8_TEST_MODEL). A
// missing dependency is a SKIP, not a failure.
//
// M8_BINARY_DIR is prepended to PATH for the duration of each test, so the
// tool_* binaries this build produced are the ones the agent finds. That makes
// the suite exercise the real discovery path without requiring `make install`.
//
// Run with:  make integration-test   (or  ctest --test-dir build -L integration)

#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/agent/agent.h>
#include <core/agent/agent_pool.h>
#include <core/oc/basic_ollama_client.h>
#include <core/oc/ollama_client.h>
#include <core/policy/policy.h>
#include <core/tools/tools.h>

#include <tool_test_env.h>

namespace agent {
namespace {

// True if `n` appears in `haystack` not glued to another digit, so a check for
// "21" is not satisfied by "216" or "1214".
bool mentions_number(const std::string& haystack, const std::string& n) {
  for (std::size_t pos = haystack.find(n); pos != std::string::npos;
       pos = haystack.find(n, pos + 1)) {
    const bool digit_left =
        pos > 0 and
        std::isdigit(static_cast<unsigned char>(haystack[pos - 1])) != 0;
    const std::size_t end = pos + n.size();
    const bool digit_right =
        end < haystack.size() and
        std::isdigit(static_cast<unsigned char>(haystack[end])) != 0;
    if (not digit_left and not digit_right) return true;
  }
  return false;
}

struct AgentBashIntegrationTest : m8test::ToolTest {
  void SetUp() override {
    const char* host_env = std::getenv("OLLAMA_HOST");
    mHost = (host_env != nullptr and *host_env != '\0')
                ? host_env
                : "http://localhost:11434";
    const char* model_env = std::getenv("M8_TEST_MODEL");
    mModel = (model_env != nullptr and *model_env != '\0') ? model_env
                                                           : "qwen3.8:27b-mlx";

    if (not oc::BasicOllamaClient(mHost).show(mModel).ok) {
      GTEST_SKIP() << "Ollama model '" << mModel << "' not reachable at "
                   << mHost << " — start Ollama and `ollama pull " << mModel
                   << "` (or set OLLAMA_HOST / M8_TEST_MODEL) to run this suite.";
    }

    m8test::ToolTest::SetUp();  // fresh temp dir, chdir into it
    mBaseReady = true;

    // The tool_* binaries this build produced, ahead of anything installed, so
    // the test measures this tree rather than whatever is on the developer's
    // PATH. Restored in TearDown.
    const char* path = std::getenv("PATH");
    mOldPath = path != nullptr ? path : "";
    setenv("PATH", (std::string(M8_BINARY_DIR) + ":" + mOldPath).c_str(), 1);

    // Each case gets its own index file inside the temp dir, so a scan never
    // touches the developer's real ~/.m8 index.
    tools::set_bash_search_index_path((dir() / "bash_search_index.json").string());

    oc::OllamaClient::configure(mModel, mHost);
    oc::OllamaClient::set_num_ctx(0);
    AgentPool::configure(/*max_agents=*/4, /*max_depth=*/0);

    AgentPool::instance().set_observer([this](const AgentEvent& event) {
      std::lock_guard<std::mutex> lock(mEventsMutex);
      mEvents.push_back(event);
    });
  }

  void TearDown() override {
    AgentPool::instance().set_observer({});
    if (mBaseReady) {
      tools::wait_for_bash_search_rescan();
      tools::set_bash_search_index_path(std::string());
      setenv("PATH", mOldPath.c_str(), 1);
      m8test::ToolTest::TearDown();
    }
  }

  // m8's tool set: one persistent shell plus command discovery. No `read`,
  // `write`, `edit` or `websearch` — those are the installed binaries now.
  AgentOptions opts() const {
    AgentOptions options;
    options.max_steps = 16;
    options.max_depth = 0;
    options.enable_subagents = false;
    options.enable_bash_repl = true;
    options.enable_bash_search = true;
    options.enable_file_tools = false;
    options.enable_web_search = false;
    options.enable_memory = false;
    options.skills_dir = ".m8/skills";
    options.context_window_tokens = 0;
    options.context_summarize_at_tokens = 100'000'000;  // never mid-test
    return options;
  }

  AgentResult run(const std::string& objective) {
    const policy::YoloPolicy pol;
    const std::string id = AgentPool::instance().register_root("root");
    Agent agent(opts(), pol, id, "", 0);
    return agent.run_turn(objective);
  }

  std::vector<AgentEvent> events() const {
    std::lock_guard<std::mutex> lock(mEventsMutex);
    return mEvents;
  }

  bool called_tool(const std::string& name) const {
    for (const AgentEvent& event : events()) {
      if (event.kind == AgentEvent::Kind::ToolCall and
          event.tool_name == name) {
        return true;
      }
    }
    return false;
  }

  bool called_shell() const { return called_tool("bash_repl"); }

  // Every shell command the agent ran, concatenated — for asserting that it
  // reached for a particular binary rather than rolling its own.
  std::string shell_commands() const {
    std::string all;
    for (const AgentEvent& event : events()) {
      if (event.kind == AgentEvent::Kind::ToolCall and
          event.tool_name == "bash_repl") {
        all += event.summary;
        all += '\n';
      }
    }
    return all;
  }

  std::string tool_output() const {
    std::string all;
    for (const AgentEvent& event : events()) {
      if (event.kind == AgentEvent::Kind::ToolResult) {
        all += event.text;
        all += '\n';
      }
    }
    return all;
  }

protected:
  std::string mHost;
  std::string mModel;
  std::string mOldPath;
  bool mBaseReady = false;
  mutable std::mutex mEventsMutex;
  std::vector<AgentEvent> mEvents;
};

TEST_F(AgentBashIntegrationTest, FindsDetailInRepoFile) {
  write_file("src/config.h",
             "#ifndef CONFIG_H\n"
             "#define CONFIG_H\n"
             "// Tuning knobs for the widget subsystem.\n"
             "constexpr int kMaxRetries = 7;\n"
             "constexpr int kBufferBytes = 4096;\n"
             "constexpr int kPollIntervalMs = 250;\n"
             "#endif\n");
  init_git_repo();

  const AgentResult result = run(
      "This directory is a code repository. Read the file src/config.h and tell "
      "me the integer value of the constant named kBufferBytes.");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(called_shell()) << "the agent's only substrate is the shell";
  EXPECT_TRUE(mentions_number(result.conclusion, "4096"))
      << "conclusion: " << result.conclusion;
}

// The whole point of bash_repl over one-shot bash: the model should be able to
// rely on state set in one call still being there in the next.
TEST_F(AgentBashIntegrationTest, ShellStatePersistsAcrossCalls) {
  const AgentResult result = run(
      "Do this in two separate steps, using the shell both times. First, set a "
      "shell variable named WIDGET_COUNT to 1071 and do nothing else. Second, "
      "in a later shell call, print the value of WIDGET_COUNT without setting "
      "it again, and report what it printed.");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(called_shell());
  const std::string haystack = result.conclusion + "\n" + tool_output();
  EXPECT_TRUE(mentions_number(haystack, "1071")) << haystack;
}

TEST_F(AgentBashIntegrationTest, CreatesMarkdownFile) {
  const AgentResult result = run(
      "Create a markdown file named notes.md in the current directory. Give it "
      "a level-1 heading that reads 'Release Notes', followed by a bullet list "
      "with exactly three items: alpha, beta, and gamma.");

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_TRUE(exists("notes.md")) << "the agent did not create notes.md";
  const std::string md = read_back("notes.md");
  EXPECT_NE(md.find("# Release Notes"), std::string::npos) << md;
  EXPECT_NE(md.find("alpha"), std::string::npos) << md;
  EXPECT_NE(md.find("beta"), std::string::npos) << md;
  EXPECT_NE(md.find("gamma"), std::string::npos) << md;
}

// bash_search is how the agent learns what this machine can do. Asking for a
// capability it has no built-in tool for should send it there.
TEST_F(AgentBashIntegrationTest, DiscoversACommandWithBashSearch) {
  const AgentResult result = run(
      "You have no built-in tool for searching file contents. Use bash_search "
      "to find which commands on this machine can search text inside files, "
      "then name one of them.");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(called_tool("bash_search"))
      << "the objective names the tool outright";
}

TEST_F(AgentBashIntegrationTest, LoadsAndFollowsASkillFromTheCatalog) {
  write_file(
      ".m8/skills/rot13/SKILL.md",
      "---\n"
      "name: rot13\n"
      "description: Apply the ROT13 substitution cipher to a piece of text. Use "
      "when the user asks to rot13, decode a rot13 string, or apply a Caesar "
      "shift of 13.\n"
      "---\n\n"
      "# rot13\n\n"
      "Run the text through `tr 'A-Za-z' 'N-ZA-Mn-za-m'` in the shell. Print "
      "only the transformed text, nothing else.\n");

  const AgentResult result =
      run("Decode this rot13 string and tell me what it says: 'Uryyb Jbeyq'");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(called_tool("skill"))
      << "the agent should load the rot13 skill from the catalog";
  EXPECT_TRUE(called_shell()) << "the skill says to run the cipher in the shell";
  EXPECT_NE(result.conclusion.find("Hello World"), std::string::npos)
      << "conclusion: " << result.conclusion;
}

}  // namespace
}  // namespace agent
