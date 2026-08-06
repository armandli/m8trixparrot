#include <core/workspace_context.h>

#include <filesystem>
#include <memory>

#include <git2.h>

namespace agent {

namespace {

// git_libgit2_init() must run once before any git2 call, and is not itself
// thread-safe, so it happens here via a function-local static (C++11
// guarantees the initializer runs exactly once, safely).
void ensure_git2_initialized() {
  static const int init_result = git_libgit2_init();
  (void)init_result;
}

using RepoPtr = std::unique_ptr<git_repository, decltype(&git_repository_free)>;
using RefPtr = std::unique_ptr<git_reference, decltype(&git_reference_free)>;
using StatusListPtr = std::unique_ptr<git_status_list, decltype(&git_status_list_free)>;

RepoPtr open_repo(const std::string& start_path) {
  git_repository* repo = nullptr;
  if (git_repository_open_ext(&repo, start_path.c_str(), 0, nullptr) != 0) {
    return RepoPtr(nullptr, &git_repository_free);
  }
  return RepoPtr(repo, &git_repository_free);
}

std::string trim_trailing_slash(std::string path) {
  if (not path.empty() and path.back() == '/') {
    path.pop_back();
  }
  return path;
}

std::string detect_repo_root(git_repository* repo) {
  const char* workdir = git_repository_workdir(repo);
  if (workdir != nullptr) {
    return trim_trailing_slash(workdir);
  }
  // Bare repository: fall back to the .git directory itself.
  const char* git_dir = git_repository_path(repo);
  return git_dir != nullptr ? trim_trailing_slash(git_dir) : std::string();
}

std::string short_oid(const git_oid* oid) {
  char buf[8] = {};
  git_oid_tostr(buf, sizeof(buf), oid);
  return std::string(buf);
}

std::string strip_prefix(std::string value, const std::string& prefix) {
  if (value.rfind(prefix, 0) == 0) {
    value.erase(0, prefix.size());
  }
  return value;
}

std::string detect_branch(git_repository* repo) {
  git_reference* raw_head = nullptr;
  const int head_result = git_repository_head(&raw_head, repo);

  if (head_result == 0) {
    RefPtr head(raw_head, &git_reference_free);
    if (git_repository_head_detached(repo) == 1) {
      return "HEAD detached at " + short_oid(git_reference_target(head.get()));
    }
    const char* shorthand = git_reference_shorthand(head.get());
    return shorthand != nullptr ? std::string(shorthand) : std::string();
  }

  if (head_result == GIT_EUNBORNBRANCH) {
    // Fresh repo, no commits yet: git_repository_head() can't resolve, but
    // HEAD's symbolic target still names the branch that will be created.
    git_reference* raw_symbolic = nullptr;
    if (git_reference_lookup(&raw_symbolic, repo, "HEAD") == 0) {
      RefPtr symbolic(raw_symbolic, &git_reference_free);
      const char* target = git_reference_symbolic_target(symbolic.get());
      if (target != nullptr) {
        return strip_prefix(target, "refs/heads/");
      }
    }
  }

  return std::string();
}

char index_status_char(unsigned int status) {
  if (status & GIT_STATUS_INDEX_NEW) return 'A';
  if (status & GIT_STATUS_INDEX_MODIFIED) return 'M';
  if (status & GIT_STATUS_INDEX_DELETED) return 'D';
  if (status & GIT_STATUS_INDEX_RENAMED) return 'R';
  if (status & GIT_STATUS_INDEX_TYPECHANGE) return 'T';
  return ' ';
}

char worktree_status_char(unsigned int status) {
  if (status & GIT_STATUS_WT_MODIFIED) return 'M';
  if (status & GIT_STATUS_WT_DELETED) return 'D';
  if (status & GIT_STATUS_WT_RENAMED) return 'R';
  if (status & GIT_STATUS_WT_TYPECHANGE) return 'T';
  return ' ';
}

std::string format_status_line(const git_status_entry& entry) {
  const unsigned int status = entry.status;

  if (status & GIT_STATUS_WT_NEW) {
    const char* path =
        entry.index_to_workdir != nullptr ? entry.index_to_workdir->new_file.path : "";
    return "?? " + std::string(path);
  }

  const char x = index_status_char(status);
  const char y = worktree_status_char(status);

  if (x == 'R' and entry.head_to_index != nullptr) {
    return std::string("R  ") + entry.head_to_index->old_file.path + " -> " +
           entry.head_to_index->new_file.path;
  }
  if (y == 'R' and entry.index_to_workdir != nullptr) {
    return std::string("R  ") + entry.index_to_workdir->old_file.path + " -> " +
           entry.index_to_workdir->new_file.path;
  }

  const char* path = nullptr;
  if (x != ' ' and entry.head_to_index != nullptr) {
    path = entry.head_to_index->new_file.path;
  } else if (entry.index_to_workdir != nullptr) {
    path = entry.index_to_workdir->new_file.path;
  }

  std::string line;
  line += x;
  line += y;
  line += ' ';
  line += (path != nullptr ? path : "");
  return line;
}

std::string detect_status(git_repository* repo) {
  git_status_options opts = GIT_STATUS_OPTIONS_INIT;
  opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
               GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
               GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
               GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;

  git_status_list* raw_list = nullptr;
  if (git_status_list_new(&raw_list, repo, &opts) != 0) {
    return std::string();
  }
  StatusListPtr list(raw_list, &git_status_list_free);

  std::string result;
  const size_t count = git_status_list_entrycount(list.get());
  for (size_t i = 0; i < count; ++i) {
    const git_status_entry* entry = git_status_byindex(list.get(), i);
    if (entry == nullptr) continue;
    if (not result.empty()) result += "\n";
    result += format_status_line(*entry);
  }
  return result;
}

}  // namespace

WorkspaceContext WorkspaceContext::from_environment(const std::string& start_path) {
  ensure_git2_initialized();

  WorkspaceContext context;
  context.cwd = std::filesystem::current_path().string();

  RepoPtr repo = open_repo(start_path);
  if (not repo) {
    return context;
  }

  context.in_git_repo = true;
  context.repo_root = detect_repo_root(repo.get());
  context.git_branch = detect_branch(repo.get());
  context.git_status = detect_status(repo.get());
  return context;
}

}  // namespace agent
