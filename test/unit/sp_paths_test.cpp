// Where sp keeps its files. Resolution is a pure function of the home
// directory, the XDG environment and PATH, so most of these are one-liners;
// the filesystem only appears for directory creation and migration.

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include <sp_paths.h>

namespace sp {
namespace {

// A throwaway home, so these never read or write the real one.
struct SpPathsTest : ::testing::Test {
  std::filesystem::path home;

  void SetUp() override {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    home = std::filesystem::temp_directory_path() /
           ("sp-paths-" + std::string(info->name()));
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
  }

  std::string at(const std::string& relative) const {
    return (home / relative).string();
  }

  void write(const std::string& relative, const std::string& contents) const {
    const std::filesystem::path target = home / relative;
    std::filesystem::create_directories(target.parent_path());
    std::ofstream(target) << contents;
  }

  SpPaths resolve(const XdgEnv& xdg = {},
                  const std::string& path_env = "/usr/bin") const {
    return resolve_sp_paths(home.string(), xdg, path_env);
  }
};

// ──────────────────────────── the XDG defaults ─────────────────────────────

TEST_F(SpPathsTest, DefaultsToTheXdgBaseDirectories) {
  const SpPaths paths = resolve();

  EXPECT_EQ(paths.data, at(".local/share/sp"));
  EXPECT_EQ(paths.config, at(".config/sp"));
  EXPECT_EQ(paths.state, at(".local/state/sp"));
  EXPECT_EQ(paths.cache, at(".cache/sp"));
}

TEST_F(SpPathsTest, ComposesEveryPathFromItsRoot) {
  const SpPaths paths = resolve();

  EXPECT_EQ(paths.bin(), at(".local/share/sp/bin"));
  EXPECT_EQ(paths.src(), at(".local/share/sp/src"));
  EXPECT_EQ(paths.trash(), at(".local/share/sp/trash"));
  EXPECT_EQ(paths.memory(), at(".local/share/sp/memory.m8db"));
  EXPECT_EQ(paths.config_file(), at(".config/sp/config"));
  EXPECT_EQ(paths.sessions(), at(".local/state/sp/sessions"));
  EXPECT_EQ(paths.search_index(), at(".cache/sp/bash_search_index.json"));
}

TEST_F(SpPathsTest, EachXdgVariableIsHonouredIndependently) {
  XdgEnv xdg;
  xdg.data_home = "/opt/data";
  EXPECT_EQ(resolve(xdg).data, "/opt/data/sp");
  // The others still fall back.
  EXPECT_EQ(resolve(xdg).config, at(".config/sp"));

  xdg = {};
  xdg.config_home = "/opt/conf";
  EXPECT_EQ(resolve(xdg).config, "/opt/conf/sp");

  xdg = {};
  xdg.state_home = "/opt/state";
  EXPECT_EQ(resolve(xdg).state, "/opt/state/sp");

  xdg = {};
  xdg.cache_home = "/opt/cache";
  EXPECT_EQ(resolve(xdg).cache, "/opt/cache/sp");
}

// The spec says a relative value must be ignored, not resolved against
// anything — and an empty one is the usual way it shows up in a shell rc.
TEST_F(SpPathsTest, IgnoresARelativeOrEmptyXdgValue) {
  XdgEnv relative;
  relative.data_home = "relative/path";
  EXPECT_EQ(resolve(relative).data, at(".local/share/sp"));

  XdgEnv empty;
  empty.data_home = "";
  EXPECT_EQ(resolve(empty).data, at(".local/share/sp"));
}

TEST_F(SpPathsTest, TrimsATrailingSlashFromAnXdgValue) {
  XdgEnv xdg;
  xdg.data_home = "/opt/data/";
  EXPECT_EQ(resolve(xdg).data, "/opt/data/sp");
}

// ───────────────────────────── PATH matching ───────────────────────────────

// Unlike the old ~/.local/bin, this directory is usually NOT on PATH, so
// getting this answer right is what decides whether sp tells the user to fix
// it.
TEST_F(SpPathsTest, ReportsWhetherTheBinDirectoryIsOnPath) {
  EXPECT_FALSE(resolve({}, "/usr/bin:/bin").bin_on_path);
  EXPECT_TRUE(resolve({}, "/usr/bin:" + at(".local/share/sp/bin")).bin_on_path);
}

TEST_F(SpPathsTest, ATrailingSlashInPathStillMatches) {
  EXPECT_TRUE(resolve({}, at(".local/share/sp/bin") + "/").bin_on_path);
}

// The comparison is per entry, not a substring search over the whole variable.
TEST_F(SpPathsTest, APathEntryThatMerelyContainsTheNameDoesNotMatch) {
  EXPECT_FALSE(resolve({}, at(".local/share/sp/bin-old")).bin_on_path);
}

TEST_F(SpPathsTest, EmptyPathIsNotAMatch) {
  EXPECT_FALSE(resolve({}, "").bin_on_path);
  EXPECT_FALSE(resolve({}, "::").bin_on_path);
}

// ─────────────────────────── creating the tree ─────────────────────────────

TEST_F(SpPathsTest, EnsureCreatesTheTreeAndIsIdempotent) {
  const SpPaths paths = resolve();
  ASSERT_FALSE(std::filesystem::exists(paths.data));

  std::string error;
  EXPECT_TRUE(ensure_sp_dirs(paths, error)) << error;
  EXPECT_TRUE(std::filesystem::is_directory(paths.bin()));
  EXPECT_TRUE(std::filesystem::is_directory(paths.src()));
  EXPECT_TRUE(std::filesystem::is_directory(paths.trash()));

  EXPECT_TRUE(ensure_sp_dirs(paths, error)) << error;
  EXPECT_TRUE(error.empty());
}

// State and cache are created by whatever writes into them, not up front.
TEST_F(SpPathsTest, EnsureDoesNotCreateStateOrCache) {
  const SpPaths paths = resolve();
  std::string error;
  ASSERT_TRUE(ensure_sp_dirs(paths, error)) << error;

  EXPECT_FALSE(std::filesystem::exists(paths.state));
  EXPECT_FALSE(std::filesystem::exists(paths.cache));
}

// ───────────────────────────── the migration ───────────────────────────────

TEST_F(SpPathsTest, MovesSourcesAndConfigFromTheOldLocations) {
  write(".local/sp_development/bigfiles.cpp", "int main() {}\n");
  write(".sprc", "MODEL=qwen\n");

  const SpPaths paths = resolve();
  std::string error;
  ASSERT_TRUE(ensure_sp_dirs(paths, error)) << error;

  std::string notes;
  migrate_legacy_paths(home.string(), paths, notes);

  EXPECT_TRUE(std::filesystem::exists(paths.src() + "/bigfiles.cpp"));
  EXPECT_TRUE(std::filesystem::exists(paths.config_file()));
  EXPECT_FALSE(std::filesystem::exists(at(".sprc")));
  // The user is told, rather than finding their files moved.
  EXPECT_NE(notes.find("bigfiles.cpp"), std::string::npos) << notes;
  EXPECT_NE(notes.find(".sprc"), std::string::npos) << notes;
}

TEST_F(SpPathsTest, MigrationIsANoOpTheSecondTime) {
  write(".local/sp_development/x.cpp", "// x\n");
  const SpPaths paths = resolve();
  std::string error;
  ASSERT_TRUE(ensure_sp_dirs(paths, error)) << error;

  std::string first;
  migrate_legacy_paths(home.string(), paths, first);
  ASSERT_NE(first.find("x.cpp"), std::string::npos) << first;

  std::string second;
  migrate_legacy_paths(home.string(), paths, second);
  EXPECT_EQ(second, "") << second;
}

TEST_F(SpPathsTest, MigrationSaysNothingOnAFreshHome) {
  const SpPaths paths = resolve();
  std::string error;
  ASSERT_TRUE(ensure_sp_dirs(paths, error)) << error;

  std::string notes;
  migrate_legacy_paths(home.string(), paths, notes);
  EXPECT_EQ(notes, "") << notes;
}

// The memory database was already in the new data root, so it must be left
// exactly where it is rather than moved to itself.
TEST_F(SpPathsTest, LeavesTheMemoryDatabaseAlone) {
  write(".local/share/sp/memory.m8db", "db\n");
  const SpPaths paths = resolve();

  std::string notes;
  migrate_legacy_paths(home.string(), paths, notes);

  EXPECT_TRUE(std::filesystem::exists(paths.memory()));
  EXPECT_EQ(notes.find("memory.m8db"), std::string::npos) << notes;
}

// sp keeps no record of which files in a shared bin directory it wrote, so
// moving them would risk moving the user's own. They are reported instead.
TEST_F(SpPathsTest, LeavesAlreadyInstalledCommandsWhereTheyAre) {
  write(".local/bin/loc", "#!/usr/bin/env bash\n");
  write(".local/sp_development/x.cpp", "// x\n");
  const SpPaths paths = resolve();
  std::string error;
  ASSERT_TRUE(ensure_sp_dirs(paths, error)) << error;

  std::string notes;
  migrate_legacy_paths(home.string(), paths, notes);

  EXPECT_TRUE(std::filesystem::exists(at(".local/bin/loc")));
  EXPECT_FALSE(std::filesystem::exists(paths.bin() + "/loc"));
  EXPECT_NE(notes.find(".local/bin"), std::string::npos) << notes;
}

// That note is context for a move the user just saw, not something to act on,
// so a run with nothing to migrate must stay silent about it rather than
// repeating itself forever.
TEST_F(SpPathsTest, DoesNotMentionLegacyCommandsWhenNothingMigrated) {
  write(".local/bin/loc", "#!/usr/bin/env bash\n");
  const SpPaths paths = resolve();

  std::string notes;
  migrate_legacy_paths(home.string(), paths, notes);
  EXPECT_EQ(notes, "") << notes;
}

}  // namespace
}  // namespace sp
