#ifndef TOOL_TEST_ENV_H
#define TOOL_TEST_ENV_H

#include <cstdint>
#include <cstdlib>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <git2.h>
#include <gtest/gtest.h>

#include <core/tools/tools.h>

namespace m8test {

// Every tool resolves a relative path against the process working directory,
// and `find`/`grep` default their search root to it outright (default_path(),
// src/core/tools_search.cpp). Running them against the repo would make the
// tests depend on the repo's own contents, so each test gets an empty
// directory and runs inside it.
//
// The cwd is process-global, so this is only safe because gtest runs cases
// sequentially within a process. Don't add a threaded test to this binary
// without revisiting it.
struct ToolTest : ::testing::Test {
  void SetUp() override {
    mOldCwd = std::filesystem::current_path();

    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string name = std::string(info->test_suite_name()) + "-" +
                             info->name() + "-" +
                             std::to_string(::testing::UnitTest::GetInstance()
                                                ->random_seed());

    mDir = std::filesystem::temp_directory_path() /
           ("m8-test-" + sanitize(name));
    std::filesystem::remove_all(mDir);
    std::filesystem::create_directories(mDir);
    std::filesystem::current_path(mDir);
  }

  void TearDown() override {
    std::filesystem::current_path(mOldCwd);
    std::error_code ec;
    std::filesystem::remove_all(mDir, ec);
  }

  // Creates `path` (relative to the temp dir) and any parent directories.
  void write_file(const std::string& path, std::string_view contents) const {
    const std::filesystem::path target = mDir / path;
    if (target.has_parent_path()) {
      std::filesystem::create_directories(target.parent_path());
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

  std::string read_back(const std::string& path) const {
    std::ifstream in(mDir / path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }

  bool exists(const std::string& path) const {
    return std::filesystem::exists(mDir / path);
  }

  // An initialized repository with no commits. That is all IgnoreFilter needs:
  // it opens the repository and asks libgit2 about paths, and gitignore rules
  // are read from the worktree rather than from any committed state.
  void init_git_repo() const {
    git_libgit2_init();
    git_repository* repo = nullptr;
    ASSERT_EQ(git_repository_init(&repo, mDir.string().c_str(), 0), 0);
    git_repository_free(repo);
  }

  const std::filesystem::path& dir() const { return mDir; }

protected:
  static std::string sanitize(std::string name) {
    for (char& c : name) {
      if (not std::isalnum(static_cast<unsigned char>(c))) c = '-';
    }
    return name;
  }

  std::filesystem::path mOldCwd;
  std::filesystem::path mDir;
};

// Sets an environment variable for the lifetime of the object and restores the
// previous state (value or absence) on destruction. gtest runs cases
// sequentially in one process, so this is safe.
struct ScopedEnv {
  ScopedEnv(const std::string& name, const std::string& value) : mName(name) {
    capture();
    ::setenv(mName.c_str(), value.c_str(), 1);
  }

  // The one-argument form clears the variable instead of setting it.
  explicit ScopedEnv(const std::string& name) : mName(name) {
    capture();
    ::unsetenv(mName.c_str());
  }

  ~ScopedEnv() {
    if (mHadPrior) {
      ::setenv(mName.c_str(), mPrior.c_str(), 1);
    } else {
      ::unsetenv(mName.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 protected:
  void capture() {
    if (const char* prior = std::getenv(mName.c_str())) {
      mHadPrior = true;
      mPrior = prior;
    }
  }

  std::string mName;
  std::string mPrior;
  bool mHadPrior = false;
};

// ---------------------------------------------------------------------------
// ToolArgs construction.
//
// ToolArgValue's integer alternative is int64_t, so a bare `5` would select
// the bool alternative instead. These wrappers make the intended alternative
// explicit at the call site.
// ---------------------------------------------------------------------------

inline tools::ToolArgValue str(std::string value) { return tools::ToolArgValue(std::move(value)); }
inline tools::ToolArgValue num(int64_t value) { return tools::ToolArgValue(value); }
inline tools::ToolArgValue flag(bool value) { return tools::ToolArgValue(value); }

// The array-of-{oldText,newText} alternative that `edit` takes.
inline tools::ToolArgValue edits(
    std::vector<std::pair<std::string, std::string>> pairs) {
  return tools::ToolArgValue(std::move(pairs));
}

inline tools::ToolArgs args(
    std::initializer_list<std::pair<const std::string, tools::ToolArgValue>> entries) {
  return tools::ToolArgs(entries.begin(), entries.end());
}

}  // namespace m8test

#endif  // TOOL_TEST_ENV_H
