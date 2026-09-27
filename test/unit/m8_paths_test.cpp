// Where m8 keeps its things. Resolution is a pure function of (cwd, home), so
// these tests build a directory tree under a temp root and never need a real
// environment — the same shape as sp_paths_test.cpp.

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include <m8_paths.h>

namespace m8 {
namespace {

namespace fs = std::filesystem;

struct M8PathsTest : ::testing::Test {
  void SetUp() override {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    mRoot = fs::temp_directory_path() /
            ("m8-paths-test-" + std::string(info->name()));
    fs::remove_all(mRoot);
    fs::create_directories(mRoot);
    // The temp root itself may sit under a symlink (/var -> /private/var on
    // macOS), and find_workspace_root() returns a resolved path, so compare
    // against the resolved form or every assertion fails on the prefix.
    mRoot = fs::canonical(mRoot);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(mRoot, ec);
  }

  void mkdirs(const std::string& relative) const {
    fs::create_directories(mRoot / relative);
  }

  fs::path mRoot;
};

// ─────────────────────────── find_workspace_root ───────────────────────────

TEST_F(M8PathsTest, FindsAGitMarkerInTheDirectoryItself) {
  mkdirs("repo/.git");
  EXPECT_EQ(fs::path(find_workspace_root((mRoot / "repo").string())),
            mRoot / "repo");
}

// The case the whole walk exists for: m8 started three directories deep still
// writes into the repository's .m8, not a new one beside the source file.
TEST_F(M8PathsTest, WalksUpFromASubdirectory) {
  mkdirs("repo/.git");
  mkdirs("repo/src/core/vdb");
  EXPECT_EQ(
      fs::path(find_workspace_root((mRoot / "repo/src/core/vdb").string())),
      mRoot / "repo");
}

// A .m8 with no .git is a workspace too: an agent may be run in a directory that
// is not a git repository, and once it has state there that state is the marker.
TEST_F(M8PathsTest, AcceptsAnM8MarkerWithoutGit) {
  mkdirs("plain/.m8");
  mkdirs("plain/sub");
  EXPECT_EQ(fs::path(find_workspace_root((mRoot / "plain/sub").string())),
            mRoot / "plain");
}

TEST_F(M8PathsTest, StopsAtTheNearestMarker) {
  mkdirs("outer/.git");
  mkdirs("outer/inner/.git");
  EXPECT_EQ(fs::path(find_workspace_root((mRoot / "outer/inner").string())),
            mRoot / "outer/inner");
}

TEST_F(M8PathsTest, ReturnsEmptyWhenNoMarkerIsAnywhereAbove) {
  mkdirs("bare/deeper");
  // The temp root has no .git above it on any sane machine, but a developer's
  // $TMPDIR could in principle sit inside a repository, so assert only that the
  // answer is not one of our own directories.
  const std::string found = find_workspace_root((mRoot / "bare/deeper").string());
  EXPECT_NE(fs::path(found), mRoot / "bare/deeper");
  EXPECT_NE(fs::path(found), mRoot / "bare");
}

// ──────────────────────────── resolve_m8_paths ─────────────────────────────

TEST_F(M8PathsTest, LaysEveryPathOutUnderTheWorkspaceRoot) {
  mkdirs("repo/.git");
  const M8Paths paths =
      resolve_m8_paths((mRoot / "repo/src").string(), "/home/ada");

  const std::string root = (mRoot / "repo").string();
  EXPECT_TRUE(paths.in_workspace);
  EXPECT_EQ(paths.root, root);
  EXPECT_EQ(paths.dir(), root + "/.m8");
  EXPECT_EQ(paths.config_file(), root + "/.m8/config.json");
  EXPECT_EQ(paths.skills(), root + "/.m8/skills");
  EXPECT_EQ(paths.vdb(), root + "/.m8/vdb");
  EXPECT_EQ(paths.memory(), root + "/.m8/vdb/memory.m8db");
  EXPECT_EQ(paths.sessions(), root + "/.m8/sessions");
  EXPECT_EQ(paths.api_key(), root + "/.m8/parallel_api_key");
}

// The bash_search index describes PATH, which belongs to the machine rather than
// to the repository, so it is the one thing that does NOT live under .m8.
TEST_F(M8PathsTest, KeepsTheSearchIndexInTheHomeDirectory) {
  mkdirs("repo/.git");
  const M8Paths paths =
      resolve_m8_paths((mRoot / "repo").string(), "/home/ada");
  EXPECT_EQ(paths.home_dir(), "/home/ada/.m8");
  EXPECT_EQ(paths.search_index(), "/home/ada/.m8/bash_search_index.json");
}

TEST_F(M8PathsTest, FallsBackToTheCwdOutsideAWorkspace) {
  mkdirs("bare");
  const M8Paths paths = resolve_m8_paths((mRoot / "bare").string(), "/home/ada");
  // in_workspace is what the prompt reports; root is still usable either way.
  if (not paths.in_workspace) {
    EXPECT_EQ(fs::path(paths.root), mRoot / "bare");
    EXPECT_EQ(paths.dir(), (mRoot / "bare").string() + "/.m8");
  }
}

// A process with no HOME still has to work; it just gets no shared index path,
// and the caller then leaves the bash_search default alone.
TEST_F(M8PathsTest, ToleratesAMissingHome) {
  mkdirs("repo/.git");
  const M8Paths paths = resolve_m8_paths((mRoot / "repo").string(), "");
  EXPECT_EQ(paths.home, "");
  EXPECT_EQ(paths.home_dir(), "");
  EXPECT_EQ(paths.search_index(), "");
}

// A relative HOME is a broken environment, not a path to resolve against
// something — the same rule sp_paths applies to a relative $XDG_*_HOME.
TEST_F(M8PathsTest, IgnoresARelativeHome) {
  mkdirs("repo/.git");
  const M8Paths paths =
      resolve_m8_paths((mRoot / "repo").string(), "relative/home");
  EXPECT_EQ(paths.home, "");
  EXPECT_EQ(paths.search_index(), "");
}

TEST_F(M8PathsTest, StripsTrailingSlashesFromHome) {
  mkdirs("repo/.git");
  const M8Paths paths =
      resolve_m8_paths((mRoot / "repo").string(), "/home/ada//");
  EXPECT_EQ(paths.home_dir(), "/home/ada/.m8");
}

// ───────────────────────────── ensure_m8_dirs ──────────────────────────────

TEST_F(M8PathsTest, CreatesTheWholeLayoutIncludingTheHomeDirectory) {
  mkdirs("repo/.git");
  M8Paths paths = resolve_m8_paths((mRoot / "repo").string(),
                                   (mRoot / "home").string());

  std::string error;
  ASSERT_TRUE(ensure_m8_dirs(paths, error)) << error;
  EXPECT_TRUE(fs::is_directory(paths.dir()));
  EXPECT_TRUE(fs::is_directory(paths.skills()));
  EXPECT_TRUE(fs::is_directory(paths.vdb()));
  EXPECT_TRUE(fs::is_directory(paths.sessions()));
  EXPECT_TRUE(fs::is_directory(paths.home_dir()));
}

TEST_F(M8PathsTest, IsIdempotent) {
  mkdirs("repo/.git");
  const M8Paths paths = resolve_m8_paths((mRoot / "repo").string(), "");

  std::string error;
  ASSERT_TRUE(ensure_m8_dirs(paths, error)) << error;
  ASSERT_TRUE(ensure_m8_dirs(paths, error)) << error;
  EXPECT_TRUE(error.empty());
}

// A failure has to be reported rather than thrown: m8 still runs without being
// able to persist, and the caller prints one warning instead of dying.
TEST_F(M8PathsTest, ReportsAFailureInsteadOfThrowing) {
  // A regular file where .m8 has to be a directory.
  mkdirs("repo/.git");
  std::ofstream(mRoot / "repo/.m8") << "not a directory\n";

  const M8Paths paths = resolve_m8_paths((mRoot / "repo").string(), "");
  std::string error;
  EXPECT_FALSE(ensure_m8_dirs(paths, error));
  EXPECT_NE(error.find(".m8"), std::string::npos) << error;
}

// ─────────────────────────── ensure_default_config ─────────────────────────

TEST_F(M8PathsTest, WritesADefaultConfigNamingEveryKnob) {
  mkdirs("repo/.git");
  const M8Paths paths = resolve_m8_paths((mRoot / "repo").string(), "");
  std::string error;
  ASSERT_TRUE(ensure_m8_dirs(paths, error)) << error;

  ASSERT_TRUE(ensure_default_config(paths, error)) << error;
  ASSERT_TRUE(fs::exists(paths.config_file()));

  std::ifstream in(paths.config_file());
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  // The file doubles as the documentation of what can be changed, so an empty
  // {} would be correct and useless.
  for (const char* key : {"model", "policy", "max_steps", "max_depth",
                          "max_agents", "enable_skills", "enable_subagents",
                          "enable_bash_repl", "enable_bash_search",
                          "enable_memory", "enable_web_search"}) {
    EXPECT_NE(text.find(key), std::string::npos) << key << " missing";
  }
}

// Never clobber a config the user has edited.
TEST_F(M8PathsTest, LeavesAnExistingConfigAlone) {
  mkdirs("repo/.git");
  const M8Paths paths = resolve_m8_paths((mRoot / "repo").string(), "");
  std::string error;
  ASSERT_TRUE(ensure_m8_dirs(paths, error)) << error;

  std::ofstream(paths.config_file()) << R"json({"model":"mine"})json";
  ASSERT_TRUE(ensure_default_config(paths, error)) << error;

  std::ifstream in(paths.config_file());
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  EXPECT_EQ(text, R"json({"model":"mine"})json");
}

// ───────────────────────────── find_on_path ────────────────────────────────

TEST_F(M8PathsTest, FindsAnExecutableOnThePath) {
  mkdirs("bin");
  const fs::path tool = mRoot / "bin/tool_read";
  std::ofstream(tool) << "#!/bin/sh\n";
  fs::permissions(tool, fs::perms::owner_all);

  const std::vector<std::string> found =
      find_on_path({"tool_read", "tool_grep"}, (mRoot / "bin").string());
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0], "tool_read");
}

// A file of the right name that cannot be run is not the command: advertising it
// would put a tool in the system prompt that fails the moment it is used.
TEST_F(M8PathsTest, IgnoresANonExecutableOfTheRightName) {
  mkdirs("bin");
  const fs::path tool = mRoot / "bin/tool_read";
  std::ofstream(tool) << "not a program\n";
  fs::permissions(tool, fs::perms::owner_read | fs::perms::owner_write);

  EXPECT_TRUE(find_on_path({"tool_read"}, (mRoot / "bin").string()).empty());
}

// Exec bits set, but not for us. Asking whether any of the three exec bits is
// set anywhere is a different question from whether this process can run the
// file: we own this one, so the owner class applies and the group and other
// bits are irrelevant. Only access(X_OK) gets that right.
TEST_F(M8PathsTest, IgnoresAFileThisUserCannotExecute) {
  if (::geteuid() == 0) GTEST_SKIP() << "root bypasses the exec permission bits";
  mkdirs("bin");
  const fs::path tool = mRoot / "bin/tool_read";
  std::ofstream(tool) << "#!/bin/sh\n";
  fs::permissions(tool, fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::group_exec | fs::perms::others_exec);

  EXPECT_TRUE(find_on_path({"tool_read"}, (mRoot / "bin").string()).empty());
}

TEST_F(M8PathsTest, SearchesEveryPathEntryInOrder) {
  mkdirs("a");
  mkdirs("b");
  const fs::path tool = mRoot / "b/tool_grep";
  std::ofstream(tool) << "#!/bin/sh\n";
  fs::permissions(tool, fs::perms::owner_all);

  const std::string path_env =
      (mRoot / "a").string() + ":" + (mRoot / "b").string();
  const std::vector<std::string> found = find_on_path({"tool_grep"}, path_env);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0], "tool_grep");
}

TEST_F(M8PathsTest, ToleratesTrailingSlashesAndEmptyPathEntries) {
  mkdirs("bin");
  const fs::path tool = mRoot / "bin/tool_find";
  std::ofstream(tool) << "#!/bin/sh\n";
  fs::permissions(tool, fs::perms::owner_all);

  const std::string path_env = "::" + (mRoot / "bin").string() + "/:";
  EXPECT_EQ(find_on_path({"tool_find"}, path_env).size(), 1u);
}

// An empty PATH entry means the cwd by POSIX convention. Honouring it would let
// a file named tool_grep in whatever directory m8 was started in pass for the
// installed command, so it is dropped.
TEST_F(M8PathsTest, DoesNotTreatAnEmptyEntryAsTheCurrentDirectory) {
  EXPECT_TRUE(find_on_path({"tool_read"}, "").empty());
  EXPECT_TRUE(find_on_path({"tool_read"}, ":::").empty());
}

TEST_F(M8PathsTest, ReturnsNothingForAnEmptyNameList) {
  EXPECT_TRUE(find_on_path({}, "/usr/bin:/bin").empty());
}

}  // namespace
}  // namespace m8
