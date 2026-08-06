#ifndef SANE_POLICY_H
#define SANE_POLICY_H

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <core/policy.h>
#include <core/tools.h>

namespace agent {

// The public temporary directory writes are allowed into, alongside the
// workspace. Literally /tmp — not TMPDIR, not /var/tmp.
inline constexpr const char* kPublicTempDir = "/tmp";

// Refuses the two things an unsupervised model most easily gets wrong:
//
//   - `bash` commands that invoke su or sudo (or doas / pkexec).
//   - `write` / `edit` outside the workspace directory or /tmp, and the
//     obvious `bash` equivalents — shell redirections and the common write
//     tools aimed at the same places.
//
// IMPORTANT — this is a guardrail, not a security boundary. It reads the
// command text, so anything that hides the target from a reader defeats it:
// `eval`, base64 or variable indirection, a script written and then executed,
// an interpreter one-liner (`python -c 'open("/etc/x","w")'`), a path built at
// runtime. There is a TOCTOU window too, since the check runs before the tool
// does and a symlink swapped in afterwards would not be seen. It raises the
// cost of a careless mistake; it does not contain a model that is trying to
// get out. That needs the OS: a sandbox, a container, or dropped privileges.
struct SanePolicy : PolicyInterface {
  // Allows writes under the process's current directory and /tmp.
  SanePolicy();
  // Allows writes under `workspace_root` and /tmp. The root is resolved once
  // here rather than read per call: a policy whose boundary moves when
  // something changes the process cwd is a policy you can't reason about.
  explicit SanePolicy(const std::string& workspace_root);

  std::string name() const override;
  PolicyResult verify(std::string_view tool_name,
                      const ToolArgs& args) const override;

  // The roots writes are confined to, resolved and absolute.
  const std::vector<std::filesystem::path>& write_roots() const {
    return mWriteRoots;
  }

protected:
  // True when `path` resolves to somewhere under one of the write roots.
  // Relative paths resolve against the workspace root, `.`/`..` are
  // normalized, and symlinks on the existing part of the path are followed —
  // a link inside the workspace pointing at /etc does not sneak through.
  bool path_allowed(const std::string& path) const;

  // Empty when the command is acceptable; otherwise the reason to refuse it.
  std::string inspect_command(const std::string& command) const;

private:
  std::vector<std::filesystem::path> mWriteRoots;
};

}  // namespace agent

#endif  // SANE_POLICY_H
