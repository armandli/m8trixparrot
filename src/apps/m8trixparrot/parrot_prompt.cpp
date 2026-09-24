#include <sstream>

#include <parrot_prompt.h>

namespace parrot {

std::string make_system_prompt(const agent::PromptFacts& facts) {
  std::ostringstream prompt;

  prompt << "You are a coding agent working in a terminal on the user's "
            "machine. You have "
         << facts.tool_names.size() << " tools: "
         << agent::join_tool_names(facts.tool_names)
         << ". Use them rather than guessing or asking the user to run things "
            "for you.\n\n";

  if (facts.is_root()) {
    prompt << "You are the root agent (depth 0 of max " << facts.max_depth
           << "). ";
  } else {
    prompt << "You are a subagent at depth " << facts.depth << " of max "
           << facts.max_depth
           << ". Your caller sees only your final message, not your steps. ";
  }
  prompt << facts.free_agent_slots << " of " << facts.max_agents
         << " agent slots are free.\n\n";

  prompt << "Working rules:\n" << agent::tool_guidance_rules(facts);
  prompt << agent::loop_contract_rules() << "\n";

  prompt << agent::workspace_block(facts);

  const std::string skills = agent::skills_block(facts);
  if (not skills.empty()) prompt << "\n" << skills;

  return prompt.str();
}

}  // namespace parrot
