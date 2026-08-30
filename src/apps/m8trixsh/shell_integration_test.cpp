// The generated zsh integration: what shell_is_zsh() accepts, and what
// ShellIntegration writes into its throwaway ZDOTDIR.

#include <shell_integration.h>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

namespace m8sh {
namespace {

namespace fs = std::filesystem;

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

std::string env_value(const ShellIntegration& integ, const std::string& key) {
  for (const auto& [name, value] : integ.env()) {
    if (name == key) return value;
  }
  return {};
}

TEST(ShellIsZshTest, RecognizesZshByItsBasename) {
  EXPECT_TRUE(shell_is_zsh("/bin/zsh"));
  EXPECT_TRUE(shell_is_zsh("zsh"));
  EXPECT_TRUE(shell_is_zsh("/opt/homebrew/bin/zsh"));
  EXPECT_TRUE(shell_is_zsh("-zsh"));  // a login shell's argv[0]

  EXPECT_FALSE(shell_is_zsh("/bin/bash"));
  EXPECT_FALSE(shell_is_zsh("/usr/local/bin/fish"));
  EXPECT_FALSE(shell_is_zsh("zsh-5.9"));
  EXPECT_FALSE(shell_is_zsh(""));
}

// mkdtemp() honours $TMPDIR, so pointing it at a scratch dir keeps every
// ShellIntegration's throwaway directory under one place the fixture can sweep.
struct ShellIntegrationTest : ::testing::Test {
  void SetUp() override {
    mDir = fs::temp_directory_path() /
           ("m8sh-integ-" + std::to_string(::getpid()));
    fs::create_directories(mDir);
    if (const char* prev = std::getenv("TMPDIR"); prev != nullptr) {
      mPrevTmpdir = prev;
      mHadTmpdir = true;
    }
    ::setenv("TMPDIR", mDir.c_str(), 1);
  }

  void TearDown() override {
    if (mHadTmpdir) {
      ::setenv("TMPDIR", mPrevTmpdir.c_str(), 1);
    } else {
      ::unsetenv("TMPDIR");
    }
    std::error_code ec;
    fs::remove_all(mDir, ec);
  }

  fs::path mDir;
  std::string mPrevTmpdir;
  bool mHadTmpdir = false;
};

TEST_F(ShellIntegrationTest, BuildsAZdotdirChainingToTheRealRcAndTheSnippet) {
  ShellIntegration integ{PromptConfig{}};
  ASSERT_TRUE(integ.ok()) << integ.error();

  const std::string zdotdir = env_value(integ, "ZDOTDIR");
  ASSERT_FALSE(zdotdir.empty());
  EXPECT_EQ(zdotdir + "/mode", env_value(integ, "__M8SH_MODE_FILE"));

  for (const char* name : {".zshenv", ".zprofile", ".zshrc", ".zlogin",
                           "integration.zsh", "mode"}) {
    EXPECT_TRUE(fs::exists(zdotdir + "/" + name)) << name;
  }

  const std::string zshrc = slurp(zdotdir + "/.zshrc");
  EXPECT_NE(zshrc.find("/.zshrc"), std::string::npos);  // sources the real one
  EXPECT_NE(zshrc.find("integration.zsh"), std::string::npos);

  const std::string snippet = slurp(zdotdir + "/integration.zsh");
  EXPECT_NE(snippet.find("__m8sh_accept"), std::string::npos);
  EXPECT_NE(snippet.find("]5171;"), std::string::npos);
  EXPECT_NE(snippet.find("add-zsh-hook precmd"), std::string::npos);

  EXPECT_EQ("shell\n", slurp(zdotdir + "/mode"));
}

TEST_F(ShellIntegrationTest, SetModeRewritesTheModeFile) {
  ShellIntegration integ{PromptConfig{}};
  ASSERT_TRUE(integ.ok()) << integ.error();
  const std::string mode_file = env_value(integ, "__M8SH_MODE_FILE");

  integ.set_mode("ai");
  EXPECT_EQ("ai\n", slurp(mode_file));
  integ.set_mode("ai-ask");
  EXPECT_EQ("ai-ask\n", slurp(mode_file));
  integ.set_mode("shell");
  EXPECT_EQ("shell\n", slurp(mode_file));
}

TEST_F(ShellIntegrationTest, PromptConfigValuesAreEmbeddedInTheSnippet) {
  PromptConfig cfg;
  cfg.format = "MYFMT %tag %git";
  cfg.shell_tag = "<<SHELL>>";
  cfg.ai_tag = "<<AI>>";
  cfg.ask_tag = "<<ASK>>";
  ShellIntegration integ{cfg};
  ASSERT_TRUE(integ.ok()) << integ.error();

  const std::string snippet =
      slurp(env_value(integ, "ZDOTDIR") + "/integration.zsh");
  EXPECT_NE(snippet.find("MYFMT %tag %git"), std::string::npos);
  EXPECT_NE(snippet.find("<<SHELL>>"), std::string::npos);
  EXPECT_NE(snippet.find("<<AI>>"), std::string::npos);
  EXPECT_NE(snippet.find("<<ASK>>"), std::string::npos);
}

TEST_F(ShellIntegrationTest, ASingleQuoteInAConfigValueIsEscaped) {
  PromptConfig cfg;
  cfg.shell_tag = "it's here";
  ShellIntegration integ{cfg};
  ASSERT_TRUE(integ.ok()) << integ.error();

  const std::string snippet =
      slurp(env_value(integ, "ZDOTDIR") + "/integration.zsh");
  // Assigned as  __M8SH_TAG_SHELL='it'\''s here'
  EXPECT_NE(snippet.find("'it'\\''s here'"), std::string::npos);
}

TEST_F(ShellIntegrationTest, RemovesItsDirectoryOnDestruction) {
  std::string zdotdir;
  {
    ShellIntegration integ{PromptConfig{}};
    ASSERT_TRUE(integ.ok()) << integ.error();
    zdotdir = env_value(integ, "ZDOTDIR");
    EXPECT_TRUE(fs::exists(zdotdir));
  }
  EXPECT_FALSE(fs::exists(zdotdir));
}

}  // namespace
}  // namespace m8sh
