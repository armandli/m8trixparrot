#include <cstdint>
#include <cstdio>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <iterator>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <CLI/CLI.hpp>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <common/transcript_view.h>
#include <core/agent.h>
#include <core/agent_pool.h>
#include <core/agent_settings.h>
#include <core/policy.h>
#include <core/sane_policy.h>
#include <core/tools.h>

namespace f = ftxui;

namespace {

// Runs `ollama list` and returns the model names in its NAME column.
// On failure to launch/read/reap the process, sets `command_ok` to false.
std::vector<std::string> list_ollama_models(bool& command_ok) {
  std::vector<std::string> models;
  FILE* pipe = popen("ollama list 2>/dev/null", "r");
  if (pipe == nullptr) {
    command_ok = false;
    return models;
  }

  std::string output;
  char buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    output.append(buffer, n);
  }
  command_ok = (pclose(pipe) == 0);

  std::istringstream stream(output);
  std::string line;
  bool is_header = true;
  while (std::getline(stream, line)) {
    if (is_header) {
      is_header = false;
      continue;
    }
    std::istringstream line_stream(line);
    std::string name;
    if (line_stream >> name) {
      models.push_back(name);
    }
  }
  return models;
}

const char* kHelpText =
    "/help     show this message\n"
    "/session  show the current session id\n"
    "/context  show context token usage and the auto-summarize threshold\n"
    "/skills   list available skills (and re-scan the skills directory)\n"
    "/reset    start a new session\n"
    "/quit     exit\n"
    "\n"
    "click a > / v header to fold or unfold that tool call, group, or subagent\n"
    "Ctrl+T    fold or unfold everything at once\n"
    "Ctrl+G    while subagents run: toggle the pane grid / the transcript";

}  // namespace

int main(int argc, char** argv) {
  // The transcript model and its renderers now live in the shared agentui
  // library; pull the names in for this whole function.
  using namespace agentui;

  CLI::App app{"m8trixparrot - a coding agent over Ollama"};
  app.footer(
      "Defaults for model, policy, and the flags below may also be set in "
      "<workdir>/.m8trix/settings.json (keys: model, policy, max_steps, "
      "max_depth, max_agents, num_ctx, summarize_at, skills_dir, "
      "enable_skills, enable_subagents, enable_package_install); an "
      "explicit flag here always overrides it.");

  // .m8trix/settings.json (if present) supplies defaults for the flags below —
  // loaded before the flags are declared so CLI11's ->capture_default_str()
  // reflects it, and an explicit flag on the command line still overwrites
  // whatever the file set, since CLI11 assigns into the same variable.
  std::string settings_warning;
  const agent::StartupSettings settings =
      agent::load_startup_settings(agent::kAgentSettingsPath, settings_warning);
  if (not settings_warning.empty()) {
    std::cerr << "warning: " << settings_warning << "\n";
  }

  std::string model = settings.model.value_or("qwen3.8:27b-mlx");
  std::string resume_id;
  bool resume_latest = false;
  int max_steps = settings.max_steps.value_or(12);
  int max_depth = settings.max_depth.value_or(3);
  int max_agents = settings.max_agents.value_or(16);
  int num_ctx = settings.num_ctx.value_or(0);
  int summarize_at = settings.summarize_at.value_or(200000);
  std::string skills_dir = settings.skills_dir.value_or(".m8trix/skills");
  bool no_skills = not settings.enable_skills.value_or(true);

  app.add_option("model,-m,--model", model, "Ollama model to run the agent on")
      ->capture_default_str();
  app.add_flag("-r,--resume", resume_latest,
               "Resume the most recent session in .m8trix/sessions");
  app.add_option("-s,--session", resume_id, "Resume a specific session id");
  app.add_option("--max-steps", max_steps,
                 "Model calls allowed per turn before giving up")
      ->capture_default_str();
  app.add_option("--max-depth", max_depth,
                 "Maximum subagent nesting depth (root is 0)")
      ->capture_default_str();
  app.add_option("--max-agents", max_agents,
                 "Maximum agents live at once across the whole run")
      ->capture_default_str();
  app.add_option("--num-ctx", num_ctx,
                 "Context window to request from Ollama (0 = detect from the model)")
      ->capture_default_str();
  app.add_option("--summarize-at", summarize_at,
                 "Auto-summarize the transcript at this many tokens "
                 "(also capped at 80% of the context window)")
      ->capture_default_str();
  app.add_option("--skills-dir", skills_dir,
                 "Directory to load skills from (<dir>/<name>/SKILL.md)")
      ->capture_default_str();
  app.add_flag("--no-skills", no_skills, "Disable the skill system");

  std::string policy_name = settings.policy.value_or("sane");
  app.add_option("-p,--policy", policy_name,
                 "Permission policy: yolo (allow everything) or sane "
                 "(no su/sudo, writes confined to the working directory "
                 "and /tmp)")
      ->capture_default_str()
      ->check(CLI::IsMember({"yolo", "sane"}));

  CLI11_PARSE(app, argc, argv);

  bool list_command_ok = true;
  const std::vector<std::string> available_models =
      list_ollama_models(list_command_ok);
  if (not list_command_ok) {
    std::cerr << "error: failed to run 'ollama list' to verify model availability\n"
                  "(is the ollama CLI installed and on PATH?)\n";
    return 1;
  }
  if (std::find(available_models.begin(), available_models.end(), model) ==
      available_models.end()) {
    std::cerr << "error: model '" << model
              << "' is not available. Run 'ollama list' to see available models.\n";
    return 1;
  }

  // Agent only knows PolicyInterface, so which policy is in force is decided
  // here and nowhere else.
  const agent::YoloPolicy yolo_policy;
  const agent::SanePolicy sane_policy;
  const agent::PolicyInterface& policy =
      policy_name == "sane"
          ? static_cast<const agent::PolicyInterface&>(sane_policy)
          : static_cast<const agent::PolicyInterface&>(yolo_policy);

  agent::OllamaClient::configure(model);
  agent::AgentPool::configure(max_agents, max_depth);

  // Bring Python up (main thread, before any agent thread touches it) and
  // build/activate this workspace's .m8trixenv. A hard failure is worth
  // stopping for: the agent would otherwise run against the base interpreter
  // and package_install would be broken.
  const agent::VenvBootstrap venv = agent::create_workspace_venv();
  if (venv.status == agent::VenvBootstrap::Status::Failed) {
    std::cerr << "error: could not create the .m8trixenv virtualenv at "
              << venv.venv_dir << "\n       " << venv.detail << "\n";
    return 1;
  }
  if (venv.status == agent::VenvBootstrap::Status::NotAProject) {
    std::cerr << "note: launch directory is not a project (no .git, .m8trix, "
                 "pyproject.toml or requirements.txt here or in any parent); "
                 "skipping .m8trixenv and running against the base Python\n";
  }

  int64_t window = num_ctx;
  if (window <= 0) {
    window = agent::OllamaClient::instance().context_length(model);
    if (window <= 0) {
      std::cerr << "warning: could not detect the context length for '" << model
                << "'; auto-summarizing at a flat " << summarize_at
                << " tokens\n";
    }
  }
  if (window > 0) agent::OllamaClient::set_num_ctx(window);

  agent::AgentOptions options;
  options.max_steps = max_steps;
  options.max_depth = max_depth;
  options.max_agents = max_agents;
  options.context_window_tokens = static_cast<int>(std::max<int64_t>(0, window));
  options.context_summarize_at_tokens = summarize_at;
  options.skills_dir = skills_dir;
  options.enable_skills = not no_skills;
  // No CLI flag for these two — settings.json is their only knob.
  options.enable_subagents =
      settings.enable_subagents.value_or(options.enable_subagents);
  options.enable_package_install =
      settings.enable_package_install.value_or(options.enable_package_install);

  const std::string root_id = agent::AgentPool::instance().register_root("root");
  agent::Agent root_agent(options, policy, root_id, "", 0);

  std::mutex mutex;
  std::list<TranscriptNode> transcript;
  // Where each agent's events land. Root -> &transcript; a subagent ->
  // &<its Subagent node>.children. Guarded by `mutex`, like `transcript`.
  std::unordered_map<std::string, std::list<TranscriptNode>*> agent_containers;
  std::unordered_map<std::string, TranscriptNode*> subagent_nodes;
  agent_containers[root_id] = &transcript;

  bool waiting_for_reply = false;
  float scroll_y = 1.0f;  // 0 = top of history, 1 = bottom (most recent).
  f::Box viewport_box = kNoBox;
  std::atomic<bool> shutting_down{false};
  int64_t ctx_tokens = 0;   // Root context usage, from ContextUsage events.
  int64_t ctx_budget = 0;   // The auto-summarize threshold. Both under `mutex`.

  // Subagent ids currently running, in spawn order. An unordered_map iteration
  // order is unstable and would make panes jump between frames. Ids, not
  // TranscriptNode*: on ContextSummarized the owning std::list is cleared, so a
  // cached pointer could dangle; resolving through `subagent_nodes` at render
  // time lets a pruned pane simply vanish. Guarded by `mutex`.
  std::vector<std::string> running_agents;
  // Ctrl+G while subagents run: show the transcript instead of the pane grid.
  // Reset to false whenever `running_agents` empties. Guarded by `mutex`.
  bool force_conversation_view = false;

  auto screen = f::App::Fullscreen();

  // One process-wide observer for every agent in the tree. Set before any turn
  // runs; the callback fires on arbitrary agent threads.
  agent::AgentPool::instance().set_observer([&](const agent::AgentEvent& event) {
    if (shutting_down.load()) return;
    {
      std::lock_guard<std::mutex> lock(mutex);

      std::list<TranscriptNode>* container = &transcript;
      if (auto it = agent_containers.find(event.agent_id);
          it != agent_containers.end()) {
        container = it->second;
      }

      switch (event.kind) {
        case agent::AgentEvent::Kind::Assistant:
          add_node(*container, TranscriptNode::Kind::Assistant, event.text);
          break;

        case agent::AgentEvent::Kind::ToolCall: {
          ToolSegment& segment = open_segment(*container);
          segment.tool_name = event.tool_name;
          segment.summary = event.summary;
          break;
        }

        case agent::AgentEvent::Kind::ToolResult:
        case agent::AgentEvent::Kind::Denied: {
          if (not container->empty() and
              container->back().kind == TranscriptNode::Kind::ToolGroup and
              not container->back().segments.empty()) {
            ToolSegment& segment = container->back().segments.back();
            segment.result = event.text;
            segment.denied = (event.kind == agent::AgentEvent::Kind::Denied);
          }
          break;
        }

        case agent::AgentEvent::Kind::Error:
          add_node(*container, TranscriptNode::Kind::Error, event.text);
          break;

        case agent::AgentEvent::Kind::Notice:
          add_node(*container, TranscriptNode::Kind::Notice, event.text);
          break;

        case agent::AgentEvent::Kind::SubagentStart: {
          std::list<TranscriptNode>* parent = &transcript;
          if (auto it = agent_containers.find(event.parent_id);
              it != agent_containers.end()) {
            parent = it->second;
          }
          TranscriptNode node;
          node.kind = TranscriptNode::Kind::Subagent;
          node.agent_id = event.agent_id;
          node.objective = event.summary;
          node.depth = event.depth;
          node.expanded = true;
          parent->push_back(std::move(node));
          TranscriptNode& stored = parent->back();
          agent_containers[event.agent_id] = &stored.children;
          subagent_nodes[event.agent_id] = &stored;
          running_agents.push_back(event.agent_id);
          break;
        }

        case agent::AgentEvent::Kind::SubagentDone: {
          if (auto it = subagent_nodes.find(event.agent_id);
              it != subagent_nodes.end()) {
            it->second->done = true;
            it->second->ok = event.ok;
            collapse_subtree(*it->second);
          }
          std::erase(running_agents, event.agent_id);
          break;
        }

        case agent::AgentEvent::Kind::ContextUsage:
          if (event.depth == 0) {
            ctx_tokens = event.tokens;
            ctx_budget = event.token_budget;
          }
          break;

        case agent::AgentEvent::Kind::ContextSummarized: {
          // Compact this agent's view to match its now-summarized transcript.
          for (TranscriptNode& node : *container) {
            forget_subtree(node, subagent_nodes, agent_containers);
          }
          container->clear();
          if (event.depth == 0) {
            // Everything lived under the root; rebuild the routing tables.
            subagent_nodes.clear();
            agent_containers.clear();
            agent_containers[root_id] = &transcript;
            container = &transcript;
          }
          // A subagent whose node was just pruned (or already finished) is no
          // longer a live pane.
          std::erase_if(running_agents, [&](const std::string& id) {
            auto it = subagent_nodes.find(id);
            return it == subagent_nodes.end() or it->second->done;
          });
          add_node(*container, TranscriptNode::Kind::Notice,
                   "\xe2\x94\x80\xe2\x94\x80 " + event.text +
                       " \xe2\x94\x80\xe2\x94\x80");
          break;
        }
      }
      scroll_y = 1.0f;
    }
    screen.PostEvent(f::Event::Custom);
  });

  if (resume_latest or not resume_id.empty()) {
    const agent::SessionResult resumed = root_agent.resume(resume_id);
    if (resumed.ok) {
      const agent::AgentResult& tree = resumed.session.result;
      const bool empty_tree = tree.objective.empty() and
                              tree.conclusion.empty() and tree.error.empty() and
                              tree.children.empty();
      add_node(transcript, TranscriptNode::Kind::Notice,
               "resumed session " + resumed.session.session_id +
                   " (read-only; your next message starts a fresh session)");
      if (empty_tree) {
        add_node(transcript, TranscriptNode::Kind::Notice,
                 "this session has no saved result");
      } else {
        if (not tree.objective.empty()) {
          add_node(transcript, TranscriptNode::Kind::User, tree.objective);
        }
        render_result_body(transcript, tree);
      }
    } else {
      add_node(transcript, TranscriptNode::Kind::Error,
               "could not resume: " + resumed.error);
    }
  }

  std::string input_value;
  int input_cursor = 0;

  // Input history is only ever touched from the main thread (unlike the
  // fields above, which the background worker also writes), so it needs no
  // mutex.
  std::vector<std::string> input_history;  // Most-recent-last; capped below.
  constexpr size_t kMaxInputHistory = 100;
  size_t history_index = 0;  // == input_history.size() means "viewing the live draft".
  std::string history_draft;

  auto push_notice = [&](TranscriptNode::Kind kind, std::string text) {
    std::lock_guard<std::mutex> lock(mutex);
    add_node(transcript, kind, std::move(text));
    scroll_y = 1.0f;
  };

  // Adds `display` as the user turn and runs `objective` (usually the same
  // string; a /command expands it) on a detached thread.
  auto start_turn = [&](std::string display, std::string objective) {
    std::list<TranscriptNode>::iterator turn_begin;
    {
      std::lock_guard<std::mutex> lock(mutex);
      add_node(transcript, TranscriptNode::Kind::User, std::move(display));
      turn_begin = std::prev(transcript.end());
      waiting_for_reply = true;
      scroll_y = 1.0f;
    }

    std::thread([&root_agent, &mutex, &transcript, &waiting_for_reply,
                &scroll_y, &screen, &running_agents,
                objective = std::move(objective), turn_begin] {
      const agent::AgentResult result = root_agent.run_turn(objective);

      std::lock_guard<std::mutex> lock(mutex);
      if (not result.ok and not result.hit_step_limit and
          not result.error.empty()) {
        add_node(transcript, TranscriptNode::Kind::Error, result.error);
      }
      // The turn is done, so its tool activity and subagent blocks fold away
      // and the transcript reads as conversation again.
      for (auto it = turn_begin; it != transcript.end(); ++it) {
        collapse_subtree(*it);
      }
      // Every subagent has emitted SubagentDone by now; this is belt-and-braces
      // so a missed event can't strand the grid over the transcript.
      running_agents.clear();
      waiting_for_reply = false;
      scroll_y = 1.0f;
      screen.PostEvent(f::Event::Custom);
    }).detach();
  };

  auto send_message = [&] {
    if (input_value.empty()) {
      return;
    }

    const std::string entered = input_value;
    input_history.push_back(entered);
    if (input_history.size() > kMaxInputHistory) {
      input_history.erase(input_history.begin());
    }
    history_index = input_history.size();
    history_draft.clear();
    input_value.clear();
    input_cursor = 0;

    // !<command> runs a bash command directly, without going through the
    // model or the policy layer: it is the user's own explicit command, not
    // something the agent decided to run.
    if (entered.size() > 1 and entered[0] == '!') {
      const std::string command = entered.substr(1);
      push_notice(TranscriptNode::Kind::User, entered);
      std::thread([&push_notice, command] {
        agent::ToolArgs args;
        args["command"] = command;
        const agent::ToolResult result = agent::BashTool().execute(args);
        if (result.ok) {
          push_notice(TranscriptNode::Kind::Assistant, result.output);
        } else {
          push_notice(TranscriptNode::Kind::Error, result.error);
        }
      }).detach();
      return;
    }

    if (entered == "/quit") {
      screen.ExitLoopClosure()();
      return;
    }
    if (entered == "/help") {
      std::string help = kHelpText;
      bool header = false;
      for (const agent::SkillInfo& skill : root_agent.skill_catalog().skills) {
        if (not skill.command) continue;
        if (not header) {
          help += "\n\nskill commands:";
          header = true;
        }
        help += "\n/" + skill.name +
                (skill.argument_hint.empty() ? "" : "  " + skill.argument_hint) +
                " — " + skill.description;
      }
      push_notice(TranscriptNode::Kind::Notice, help);
      return;
    }
    if (entered == "/session") {
      const std::string id = root_agent.session_id();
      push_notice(TranscriptNode::Kind::Notice,
                  id.empty() ? "no session saved yet" : "session " + id);
      return;
    }
    if (entered == "/context") {
      const int64_t used = root_agent.context_tokens();
      const int64_t win = root_agent.context_window();
      const int64_t limit = root_agent.context_limit();
      std::string msg =
          used > 0 ? "context: ~" + std::to_string(used) + " tokens"
                   : "context: not measured yet";
      if (win > 0) msg += "  (window " + std::to_string(win) + ")";
      msg += "  auto-summarize at " + std::to_string(limit);
      push_notice(TranscriptNode::Kind::Notice, msg);
      return;
    }
    if (entered == "/reset") {
      root_agent.reset();
      {
        std::lock_guard<std::mutex> lock(mutex);
        transcript.clear();
        subagent_nodes.clear();
        agent_containers.clear();
        agent_containers[root_id] = &transcript;
        running_agents.clear();
        force_conversation_view = false;
      }
      push_notice(TranscriptNode::Kind::Notice, "started a new session");
      return;
    }
    if (entered == "/skills") {
      if (not waiting_for_reply) root_agent.reload_skills();
      const agent::SkillCatalog& catalog = root_agent.skill_catalog();
      std::string msg;
      for (const agent::SkillInfo& skill : catalog.skills) {
        msg += (skill.command ? "/" : "  ") + skill.name + " — " +
               skill.description + "\n";
      }
      for (const std::string& note : catalog.notes) msg += "(" + note + ")\n";
      push_notice(TranscriptNode::Kind::Notice,
                  msg.empty() ? "no skills found in the skills directory" : msg);
      return;
    }

    // /<name> [args] for a skill whose frontmatter opted in with command: true.
    if (entered.size() > 1 and entered[0] == '/') {
      const size_t space = entered.find(' ');
      const std::string cmd = entered.substr(
          1, space == std::string::npos ? std::string::npos : space - 1);
      const std::string cmd_args =
          space == std::string::npos ? "" : entered.substr(space + 1);
      const agent::SkillInfo* skill = root_agent.skill_catalog().find(cmd);
      if (skill != nullptr and skill->command) {
        if (not skill->argument_hint.empty() and cmd_args.empty()) {
          push_notice(TranscriptNode::Kind::Notice, "/" + cmd + "  " +
                                                        skill->argument_hint +
                                                        "\n" + skill->description);
          return;
        }
        if (waiting_for_reply) {
          push_notice(TranscriptNode::Kind::Notice,
                      "the agent is working; wait for it to finish");
          return;
        }
        std::string objective = "Run the \"" + cmd +
                                "\" skill: use the `skill` tool to load it, then "
                                "follow its SKILL.md instructions.";
        if (not cmd_args.empty()) objective += "  Arguments: " + cmd_args;
        start_turn(entered, std::move(objective));
        return;
      }
    }

    start_turn(entered, entered);
  };

  auto cursor_on_first_line = [&] {
    return input_value.substr(0, input_cursor).find('\n') == std::string::npos;
  };
  auto cursor_on_last_line = [&] {
    return input_value.find('\n', input_cursor) == std::string::npos;
  };
  auto recall_previous = [&] {
    if (input_history.empty()) {
      return;
    }
    if (history_index == input_history.size()) {
      history_draft = input_value;
    }
    if (history_index == 0) {
      return;
    }
    history_index--;
    input_value = input_history[history_index];
    input_cursor = static_cast<int>(input_value.size());
  };
  auto recall_next = [&] {
    if (history_index >= input_history.size()) {
      return;
    }
    history_index++;
    input_value = (history_index == input_history.size())
                      ? history_draft
                      : input_history[history_index];
    input_cursor = static_cast<int>(input_value.size());
  };

  f::InputOption input_option;
  input_option.content = &input_value;
  input_option.cursor_position = &input_cursor;
  input_option.placeholder =
      "Enter to send, Shift+Enter for newline, Up/Down for history, click a "
      "> header to unfold, Ctrl+T folds all, /help for commands";
  // The default transform inverts colors on focus, which would flip this back
  // to black-on-white since the input stays focused for the app's whole
  // lifetime (it's the only focusable component).
  input_option.transform = [](f::InputState state) {
    f::Element element = std::move(state.element);
    if (state.is_placeholder) {
      element |= f::dim;
    }
    return element | f::color(f::Color::White) | f::bgcolor(f::Color::Black);
  };
  auto input = f::Input(input_option);

  auto root = f::Renderer(input, [&] {
    std::vector<f::Element> lines;
    std::vector<f::Element> tiles;
    float current_scroll_y;
    int64_t header_ctx_tokens;
    int64_t header_ctx_budget;
    int running_n;
    bool show_grid;
    bool transcript_forced;  // Subagents running, but Ctrl+G chose the transcript.
    {
      std::lock_guard<std::mutex> lock(mutex);

      // Boxes are only meaningful for what this pass actually draws. Clearing
      // them first means a folded-away header can't be hit by a click landing
      // where it used to be.
      for (TranscriptNode& node : transcript) reset_boxes(node);

      // A pane whose node was pruned by a summarize, or that already finished,
      // is not live any more.
      std::erase_if(running_agents, [&](const std::string& id) {
        auto it = subagent_nodes.find(id);
        return it == subagent_nodes.end() or it->second->done;
      });
      running_n = static_cast<int>(running_agents.size());
      if (running_n == 0) force_conversation_view = false;
      show_grid = running_n > 0 and not force_conversation_view;
      transcript_forced = running_n > 0 and force_conversation_view;

      if (show_grid) {
        // The conversation isn't drawn this frame: kill its click targets so a
        // stale header box can't answer a click.
        viewport_box = kNoBox;
        for (const std::string& id : running_agents) {
          auto it = subagent_nodes.find(id);
          if (it != subagent_nodes.end()) tiles.push_back(render_pane(*it->second));
        }
      } else {
        for (TranscriptNode& node : transcript) render_node(lines, node, 0);
        if (waiting_for_reply) {
          lines.push_back(f::text("agent is working...") | f::dim);
        }
      }
      current_scroll_y = scroll_y;
      header_ctx_tokens = ctx_tokens;
      header_ctx_budget = ctx_budget;
    }

    std::string ctx_part;
    if (header_ctx_budget > 0) {
      ctx_part = "  |  ctx: " + human_tokens(header_ctx_tokens) + "/" +
                 human_tokens(header_ctx_budget);
    }
    std::string sub_part;
    if (running_n > 0) {
      sub_part = "  |  subagents: " + std::to_string(running_n) + "/" +
                 std::to_string(max_agents) +
                 (transcript_forced ? "  (Ctrl+G: grid)"
                                    : "  (Ctrl+G: transcript)");
    }

    f::Element middle;
    if (show_grid) {
      const int shown = static_cast<int>(tiles.size());
      const GridShape gs = grid_shape(std::max(shown, 1));
      std::vector<f::Elements> matrix;
      matrix.reserve(gs.rows);
      for (int r = 0; r < gs.rows; ++r) {
        f::Elements row;
        row.reserve(gs.cols);
        for (int c = 0; c < gs.cols; ++c) {
          const int idx = r * gs.cols + c;  // row-major fill
          row.push_back(idx < shown ? std::move(tiles[idx]) : empty_pane());
        }
        matrix.push_back(std::move(row));
      }
      middle = f::gridbox(std::move(matrix)) | f::flex;
    } else {
      middle = f::vbox(lines) |
               f::focusPositionRelative(0.f, current_scroll_y) |
               f::vscroll_indicator | f::yframe | f::flex |
               f::reflect(viewport_box);
    }

    return f::vbox({
               f::text("m8trixparrot  |  model: " + model + "  |  policy: " +
                       policy.name() + ctx_part + sub_part) |
                   f::bold | f::center,
               f::separator(),
               std::move(middle),
               f::separator(),
               input->Render() | f::color(f::Color::White) |
                   f::bgcolor(f::Color::Black) | f::border,
           }) |
           f::border;
  });

  root = f::CatchEvent(root, [&](f::Event event) {
    constexpr float kWheelStep = 0.1f;
    constexpr float kPageStep = 0.3f;
    static const f::Event kAltEnterCR = f::Event::Special("\x1b\r");
    static const f::Event kAltEnterLF = f::Event::Special("\x1b\n");
    static const f::Event kShiftEnterCsiU = f::Event::Special("\x1b[13;2u");
    static const f::Event kShiftEnterLegacy =
        f::Event::Special("\x1b[27;2;13~");

    // True when the pane grid, not the transcript, owns the middle region.
    // Call only while holding `mutex`.
    auto grid_active = [&] {
      return not running_agents.empty() and not force_conversation_view;
    };

    if (event == f::Event::CtrlG) {
      std::lock_guard<std::mutex> lock(mutex);
      // No grid without subagents; swallow the key anyway so it never reaches
      // the input.
      if (not running_agents.empty()) {
        force_conversation_view = not force_conversation_view;
      }
      return true;
    }
    if (event == f::Event::PageUp) {
      std::lock_guard<std::mutex> lock(mutex);
      if (grid_active()) return true;
      scroll_y = std::clamp(scroll_y - kPageStep, 0.f, 1.f);
      return true;
    }
    if (event == f::Event::PageDown) {
      std::lock_guard<std::mutex> lock(mutex);
      if (grid_active()) return true;
      scroll_y = std::clamp(scroll_y + kPageStep, 0.f, 1.f);
      return true;
    }
    if (event == f::Event::CtrlT) {
      std::lock_guard<std::mutex> lock(mutex);
      // One toggle, not a per-node inversion: whatever the tree is mostly
      // doing, everything follows the opposite.
      bool any_expanded = false;
      for (const TranscriptNode& node : transcript) {
        if (any_group_expanded(node)) {
          any_expanded = true;
          break;
        }
      }
      const bool expand = not any_expanded;
      for (TranscriptNode& node : transcript) set_all_expanded(node, expand);
      return true;
    }

    if (event.is_mouse()) {
      const f::Mouse& mouse = event.mouse();
      if (mouse.button == f::Mouse::Left and
          mouse.motion == f::Mouse::Pressed) {
        std::lock_guard<std::mutex> lock(mutex);
        // Only headers inside the scrolling viewport are live: a row laid out
        // beyond the frame still has a box, and it must not answer clicks. In
        // grid mode the transcript isn't drawn at all.
        if (not grid_active() and viewport_box.Contain(mouse.x, mouse.y)) {
          for (TranscriptNode& node : transcript) {
            if (hit_test(node, mouse.x, mouse.y)) return true;
          }
        }
        // Fall through: a click elsewhere still belongs to the input.
      }
      if (event.mouse().button == f::Mouse::WheelUp) {
        std::lock_guard<std::mutex> lock(mutex);
        if (grid_active()) return true;
        scroll_y = std::clamp(scroll_y - kWheelStep, 0.f, 1.f);
        return true;
      }
      if (event.mouse().button == f::Mouse::WheelDown) {
        std::lock_guard<std::mutex> lock(mutex);
        if (grid_active()) return true;
        scroll_y = std::clamp(scroll_y + kWheelStep, 0.f, 1.f);
        return true;
      }
    }

    if (event == f::Event::Return) {
      send_message();
      return true;
    }
    if (event == kAltEnterCR or event == kAltEnterLF or
        event == kShiftEnterCsiU or event == kShiftEnterLegacy) {
      input_value.insert(static_cast<size_t>(input_cursor), "\n");
      input_cursor += 1;
      return true;
    }
    if (event == f::Event::ArrowUp) {
      if (not cursor_on_first_line()) {
        return false;  // Let Input move the cursor up within the draft.
      }
      recall_previous();
      return true;
    }
    if (event == f::Event::ArrowDown) {
      if (not cursor_on_last_line()) {
        return false;  // Let Input move the cursor down within the draft.
      }
      recall_next();
      return true;
    }
    return false;
  });

  screen.Loop(root);
  shutting_down.store(true);
  // Drop the observer before these locals go out of scope: a subagent thread
  // that is still in flight must not call back into freed state.
  agent::AgentPool::instance().set_observer({});

  const std::string final_session = root_agent.session_id();
  if (not final_session.empty()) {
    std::cout << "session saved: " << agent::kAgentSessionDir << "/"
              << final_session << ".json\n";
  }

  return 0;
}
