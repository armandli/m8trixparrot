#include <core/tools.h>

#include <optional>
#include <string>

#include <core/tools_util.h>

namespace agent {

std::string AskUserTool::description() const {
  return R"json({"name":"ask_user","description":"Ask the operator a question and wait for their reply. Use it to get approval for a plan or a script before acting, or to resolve a genuine ambiguity you cannot settle yourself. The operator's answer is returned verbatim as the tool result. Blocks until they respond.","parameters":{"type":"object","properties":{"prompt":{"type":"string","description":"The question to show the operator. Keep it short - they answer in a small input box."}},"required":["prompt"]}})json";
}

ToolResult AskUserTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> prompt = string_arg(args, "prompt");
  if (not prompt or prompt->empty()) {
    result.error = "ask_user: missing required string argument 'prompt'";
    return result;
  }

  std::string answer = ask(*prompt);
  // An empty answer is a valid outcome (the operator dismissed the prompt);
  // saying so beats handing the model a blank tool result.
  if (answer.empty()) answer = "[operator gave no answer]";

  result.ok = true;
  result.output = std::move(answer);
  return result;
}

}  // namespace agent
