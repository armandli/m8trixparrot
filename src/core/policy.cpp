#include <core/policy.h>

#include <string>
#include <utility>

namespace agent {

PolicyResult PolicyResult::allow() {
  return PolicyResult{Decision::Allow, std::string()};
}

PolicyResult PolicyResult::deny(std::string reason) {
  if (reason.empty()) {
    reason = "the tool call was rejected by the active permission policy";
  }
  return PolicyResult{Decision::Deny, std::move(reason)};
}

std::string YoloPolicy::name() const { return "yolo"; }

PolicyResult YoloPolicy::verify(std::string_view tool_name,
                                 const ToolArgs& args) const {
  (void)tool_name;
  (void)args;
  return PolicyResult::allow();
}

}  // namespace agent
