#ifndef WORKSPACE_CONTEXT_H
#define WORKSPACE_CONTEXT_H

#include <string>

namespace agent {

struct WorkspaceContext {
  std::string cwd;
  std::string repo_root;   // Empty if start_path isn't inside a git repository.
  std::string git_branch;  // Empty if not in a repo; "HEAD detached at <sha>" if detached.
  std::string git_status;  // Raw porcelain-style text; empty if not in a repo or tree is clean.
  bool in_git_repo = false;

  // Detects the workspace context starting from `start_path` (default: the
  // process's actual current working directory). Best-effort: if
  // `start_path` isn't inside a git repository, returns a WorkspaceContext
  // with only `cwd` populated and `in_git_repo` left false — that's not
  // treated as an error, since running outside a repo is a normal case.
  static WorkspaceContext from_environment(const std::string& start_path = ".");
};

}  // namespace agent

#endif  // WORKSPACE_CONTEXT_H
