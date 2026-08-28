// Live end-to-end tests. A real Agent turn, a real Ollama model, and a
// python-only tool set (AgentOptions::enable_subagents = false). Each test hands
// the agent a plain-English objective and checks the side effect it should have
// produced — a file on disk, a value in the final message — never the exact
// wording, since the model is non-deterministic.
//
// Skipped unless Ollama is reachable with the model (default "qwen3.8:27b-mlx",
// matching src/apps/m8trixparrot/main.cpp; override with OLLAMA_HOST /
// M8_TEST_MODEL). The web test additionally needs outbound network. A missing
// dependency is a SKIP, not a failure.
//
// Run with:  make integration-test   (or  ctest --test-dir build -L integration)

#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <gtest/gtest.h>

#include <core/agent.h>
#include <core/agent_pool.h>
#include <core/basic_ollama_client.h>
#include <core/ollama_client.h>
#include <core/policy.h>
#include <core/tools.h>

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

class AgentPythonIntegrationTest : public test::ToolTest {
 protected:
  void SetUp() override {
    const char* host_env = std::getenv("OLLAMA_HOST");
    host_ = (host_env != nullptr and *host_env != '\0')
                ? host_env
                : "http://localhost:11434";
    const char* model_env = std::getenv("M8_TEST_MODEL");
    model_ = (model_env != nullptr and *model_env != '\0') ? model_env
                                                           : "qwen3.8:27b-mlx";

    if (not BasicOllamaClient(host_).show(model_).ok) {
      GTEST_SKIP() << "Ollama model '" << model_ << "' not reachable at "
                   << host_ << " — start Ollama and `ollama pull " << model_
                   << "` (or set OLLAMA_HOST / M8_TEST_MODEL) to run this suite.";
    }

    test::ToolTest::SetUp();  // fresh temp dir, chdir into it
    base_ready_ = true;

    ensure_python_ready();  // on the main thread, before any turn
    OllamaClient::configure(model_, host_);
    OllamaClient::set_num_ctx(0);
    AgentPool::configure(/*max_agents=*/4, /*max_depth=*/0);

    AgentPool::instance().set_observer([this](const AgentEvent& event) {
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.push_back(event);
    });
  }

  void TearDown() override {
    AgentPool::instance().set_observer({});
    if (base_ready_) test::ToolTest::TearDown();
  }

  AgentOptions opts() const {
    AgentOptions options;
    options.max_steps = 16;
    options.max_depth = 0;
    options.enable_subagents = false;      // python-only tool specification
    options.context_window_tokens = 0;
    options.context_summarize_at_tokens = 100'000'000;  // never mid-test
    return options;
  }

  AgentResult run(const std::string& objective) {
    const YoloPolicy policy;
    const std::string id = AgentPool::instance().register_root("root");
    Agent agent(opts(), policy, id, "", 0);
    return agent.run_turn(objective);
  }

  std::vector<AgentEvent> events() const {
    std::lock_guard<std::mutex> lock(events_mutex_);
    return events_;
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

  bool called_python() const { return called_tool("python"); }

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

  static bool network_up() {
    CURL* handle = curl_easy_init();
    if (handle == nullptr) return false;
    curl_easy_setopt(handle, CURLOPT_URL, "http://example.com/");
    curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 5L);
    const CURLcode rc = curl_easy_perform(handle);
    curl_easy_cleanup(handle);
    return rc == CURLE_OK;
  }

  std::string host_;
  std::string model_;
  bool base_ready_ = false;
  mutable std::mutex events_mutex_;
  std::vector<AgentEvent> events_;
};

TEST_F(AgentPythonIntegrationTest, FindsDetailInRepoFile) {
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
  EXPECT_TRUE(called_python()) << "the agent has no read tool; it must use python";
  EXPECT_TRUE(mentions_number(result.conclusion, "4096"))
      << "conclusion: " << result.conclusion;
}

TEST_F(AgentPythonIntegrationTest, WritesAndRunsGcd) {
  const AgentResult result = run(
      "Write a Python function that computes the greatest common divisor of two "
      "integers using the Euclidean algorithm. Then call it to compute "
      "gcd(1071, 462) and report the result.");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(called_python()) << "the agent must actually run the algorithm";
  const std::string haystack = result.conclusion + "\n" + tool_output();
  EXPECT_TRUE(mentions_number(haystack, "21")) << haystack;
}

TEST_F(AgentPythonIntegrationTest, FetchesABasicWebsite) {
  if (not network_up()) {
    GTEST_SKIP() << "no outbound network to http://example.com/";
  }

  const AgentResult result = run(
      "Fetch the web page at http://example.com/ over HTTP and tell me the "
      "exact text contained in its top-level <h1> element. Use only the Python "
      "standard library (urllib).");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_NE(result.conclusion.find("Example Domain"), std::string::npos)
      << "conclusion: " << result.conclusion;
}

TEST_F(AgentPythonIntegrationTest, CreatesMarkdownFile) {
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

TEST_F(AgentPythonIntegrationTest, LoadsAndFollowsASkillFromTheCatalog) {
  write_file(
      ".m8trix/skills/rot13/SKILL.md",
      "---\n"
      "name: rot13\n"
      "description: Apply the ROT13 substitution cipher to a piece of text. Use "
      "when the user asks to rot13, decode a rot13 string, or apply a Caesar "
      "shift of 13.\n"
      "---\n\n"
      "# rot13\n\n"
      "Use Python's `str.translate` with a table built from "
      "`string.ascii_lowercase` / `string.ascii_uppercase` rotated by 13. Print "
      "only the transformed text, nothing else.\n");

  const AgentResult result =
      run("Decode this rot13 string and tell me what it says: 'Uryyb Jbeyq'");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(called_tool("skill"))
      << "the agent should load the rot13 skill from the catalog";
  EXPECT_TRUE(called_python()) << "the skill says to run the cipher in python";
  EXPECT_NE(result.conclusion.find("Hello World"), std::string::npos)
      << "conclusion: " << result.conclusion;
}

}  // namespace
}  // namespace agent
