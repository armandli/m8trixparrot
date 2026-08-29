#ifndef POLICY_H
#define POLICY_H

#include <string>
#include <string_view>

#include <core/tools.h>

namespace agent {

// The verdict on one tool call.
//
// `reason` is written for the model, not for a log: it is what gets fed back
// in place of the tool's output when a call is refused, so it has to say what
// was refused and why in terms the model can act on ("... use `read` instead"
// beats "denied by policy").
struct PolicyResult {
  enum struct Decision : int {
    Allow,
    Deny,
    // Prompt — stop and ask the human — belongs here once there is an agent
    // loop with something to ask. The enum exists so adding it later doesn't
    // change the signature of every policy written before then.
  };

  Decision decision = Decision::Allow;
  std::string reason;  // Empty when allowed.

  bool allowed() const { return decision == Decision::Allow; }

  static PolicyResult allow();
  // An empty `reason` is replaced with a generic one: a refusal the model
  // can't read is worse than a vague one.
  static PolicyResult deny(std::string reason);
};

// Decides whether a tool call may proceed. Implementations are expected to be
// cheap and free of side effects — verify() runs ahead of every tool call.
struct PolicyInterface {
  virtual ~PolicyInterface() = default;

  // Identifies the policy in messages and logs, e.g. "yolo".
  virtual std::string name() const = 0;

  // `tool_name` is the name from the model's tool_call, matching a tool's
  // schema name in config/*.json ("bash", "read", "webfetch", ...) — including
  // names that match no tool at all, which a policy is free to refuse.
  //
  // `args` is the same map the tool's own execute() receives, so a policy
  // inspects arguments with the accessors in core/tools_util.h instead of
  // re-parsing them into a second representation that could disagree with the
  // first.
  virtual PolicyResult verify(std::string_view tool_name,
                              const ToolArgs& args) const = 0;

protected:
  // Copying through a base reference would slice. Derived policies remain free
  // to be copyable in their own right.
  PolicyInterface() = default;
  PolicyInterface(const PolicyInterface&) = default;
  PolicyInterface& operator=(const PolicyInterface&) = default;
};

// Accepts every tool call, whatever it asks for. The permissionless default,
// and the baseline the real policies are measured against.
struct YoloPolicy : PolicyInterface {
  std::string name() const override;
  PolicyResult verify(std::string_view tool_name,
                      const ToolArgs& args) const override;
};

}  // namespace agent

#endif  // POLICY_H
