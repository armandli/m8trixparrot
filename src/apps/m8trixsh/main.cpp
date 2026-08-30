#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <common/transcript_view.h>
#include <core/agent.h>
#include <core/agent_pool.h>
#include <core/agent_settings.h>
#include <core/policy.h>
#include <core/sane_policy.h>
#include <core/shell_session.h>
#include <core/tools.h>
#include <shell_integration.h>
#include <terminal_emulator.h>

namespace f = ftxui;

namespace {

enum struct Mode : int { Shell, Ai };

// Runs `ollama list` and returns the model names in its NAME column.
std::vector<std::string> list_ollama_models(bool& command_ok) {
  std::vector<std::string> models;
  FILE* pipe = popen("ollama list 2>/dev/null", "r");
  if (pipe == nullptr) {
    command_ok = false;
    return models;
  }
  std::string output;
  char buffer[4096];
  size_t n = 0;
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
    if (line_stream >> name) models.push_back(name);
  }
  return models;
}

const char* kHelpText =
    "You always type at the shell prompt. The [shell] / [m8trx] tag on the\n"
    "prompt shows where Enter goes: the shell, or the agent on the left.\n"
    "\n"
    "Shift+Tab           switch between shell mode and ai mode (also Ctrl or\n"
    "                   Opt + Shift+Tab where the terminal forwards it; rebind:\n"
    "                   MODE_SWITCH_KEY)\n"
    "\n"
    "ai mode, typed at the prompt:\n"
    "/help              show this message\n"
    "/session           show the current session id\n"
    "/context           context token usage and the auto-summarize threshold\n"
    "/skills            list available skills (and re-scan the skills dir)\n"
    "/reset             start a new session\n"
    "/quit              exit\n"
    "\n"
    "click a > / v header to fold or unfold a tool call\n"
    "Ctrl+Alt+F         fold or unfold everything\n"
    "Ctrl+Alt+J / K     scroll the ai pane\n"
    "Ctrl+Alt+H / L     resize the ai pane";

// A MODE_SWITCH_KEY setting name becomes a predicate over FTXUI events. The
// default, "shift-tab", matches the whole backtab family: plain Shift+Tab is
// `\e[Z`; a terminal in modifyOtherKeys / CSI-u mode sends `\e[1;<mod>Z`, so
// Ctrl+Shift+Tab (`\e[1;6Z`), Opt+Shift+Tab (`\e[1;4Z`), ... count too.
std::function<bool(const f::Event&)> parse_switch_key(const std::string& name) {
  const auto is_backtab = [](const f::Event& event) {
    const std::string& s = event.input();
    if (s.size() < 3 or s.front() != '\x1b' or s[1] != '[' or s.back() != 'Z') {
      return false;
    }
    for (size_t i = 2; i + 1 < s.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (std::isdigit(c) == 0 and c != ';') return false;
    }
    return true;
  };
  const auto exact = [](std::string bytes) {
    return [b = std::move(bytes)](const f::Event& event) {
      return event.input() == b;
    };
  };

  if (name == "tab") return exact("\t");
  if (name == "ctrl-]") return exact(std::string(1, '\x1d'));
  if (name == "ctrl-o") return exact(std::string(1, '\x0f'));
  if (name == "ctrl-\\") return exact(std::string(1, '\x1c'));
  if (name == "f12") return exact("\x1b[24~");
  return is_backtab;  // "shift-tab", "backtab", unset, or unrecognized
}

std::string workflow_prompt() {
  const char* home_env = std::getenv("HOME");
  const std::string home = home_env != nullptr ? home_env : "~";
  return
      "You are the AI side of an interactive shell. A human is at the terminal "
      "and can also run their own shell commands beside you. The working "
      "directory is the shell's directory as of when this turn began. Your home "
      "directory is " + home + ".\n"
      "For a request that only inspects the system (reading files, git status, "
      "searching, listing), just do it with your tools and report back "
      "concisely.\n"
      "For a request that would create, modify, move, or delete files or "
      "directories, or install or configure software:\n"
      "1. Research first with `read`, `bash`, and search. State what you found.\n"
      "2. Call `ask_user` with a short, concrete plan and wait for approval. "
      "Revise and re-ask until the human approves.\n"
      "3. On approval, `write` the steps as a shell script to " + home +
      "/bin/<name>.sh (create " + home +
      "/bin with `bash` if missing; chmod +x it).\n"
      "4. Call `ask_user` again showing the script path and full contents for a "
      "final approval.\n"
      "5. On approval, run it with `bash` and report the result.\n"
      "Never run the destructive steps before the script is approved. Keep "
      "`ask_user` prompts short - the human answers by typing at the shell "
      "prompt.";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace agentui;

  CLI::App app{"m8trixsh - an intelligent shell over Ollama"};
  app.footer(
      "Defaults for model, policy, and the flags below may also be set in "
      "~/.m8shrc, one KEY=VALUE per line (keys: MODEL, POLICY, MAX_STEPS, "
      "NUM_CTX, SUMMARIZE_AT, SKILLS_DIR, ENABLE_SKILLS, ENABLE_SUBAGENTS, "
      "ENABLE_PACKAGE_INSTALL, ENABLE_WEB_SEARCH, SHELL, MODE_SWITCH_KEY, "
      "PROMPT_FORMAT, PROMPT_SHELL_TAG, PROMPT_AI_TAG, PROMPT_ASK_TAG); an "
      "explicit flag here always overrides it.");

  std::string settings_warning;
  agent::StartupSettings settings;
  if (const char* home = std::getenv("HOME"); home != nullptr and *home != '\0') {
    settings = agent::load_shellrc_settings(
        std::string(home) + "/" + agent::kShellRcFilename, settings_warning);
  } else {
    std::cerr << "note: $HOME is unset; not reading ~/.m8shrc\n";
  }
  if (not settings_warning.empty()) {
    std::cerr << "warning: " << settings_warning << "\n";
  }

  std::string model = settings.model.value_or("qwen3.8:27b-mlx");
  std::string policy_name = settings.policy.value_or("yolo");
  int max_steps = settings.max_steps.value_or(40);
  int num_ctx = settings.num_ctx.value_or(0);
  int summarize_at = settings.summarize_at.value_or(200000);
  std::string skills_dir = settings.skills_dir.value_or(".m8trix/skills");
  bool no_skills = not settings.enable_skills.value_or(true);
  std::string shell_override = settings.shell.value_or("");
  std::string switch_key_name = settings.mode_switch_key.value_or("shift-tab");

  app.add_option("model,-m,--model", model, "Ollama model for ai mode")
      ->capture_default_str();
  app.add_option("-p,--policy", policy_name,
                 "Permission policy for ai mode: yolo (allow everything) or "
                 "sane (no su/sudo, writes confined to the working directory "
                 "and /tmp)")
      ->capture_default_str()
      ->check(CLI::IsMember({"yolo", "sane"}));
  app.add_option("--max-steps", max_steps,
                 "Model calls allowed per ai turn before giving up")
      ->capture_default_str();
  app.add_option("--num-ctx", num_ctx,
                 "Context window to request (0 = detect from the model)")
      ->capture_default_str();
  app.add_option("--summarize-at", summarize_at,
                 "Auto-summarize the ai transcript at this many tokens")
      ->capture_default_str();
  app.add_option("--skills-dir", skills_dir,
                 "Directory to load skills from (<dir>/<name>/SKILL.md)")
      ->capture_default_str();
  app.add_flag("--no-skills", no_skills, "Disable the skill system");
  app.add_option("--shell", shell_override,
                 "Shell to run in the terminal pane (default: $SHELL)");
  app.add_option("--mode-switch-key", switch_key_name,
                 "Key that toggles shell/ai mode (shift-tab, tab, ctrl-], "
                 "ctrl-o, ctrl-\\, f12)")
      ->capture_default_str();

  CLI11_PARSE(app, argc, argv);

  bool list_command_ok = true;
  const std::vector<std::string> available_models =
      list_ollama_models(list_command_ok);
  if (not list_command_ok) {
    std::cerr << "error: failed to run 'ollama list' to verify model "
                 "availability\n(is the ollama CLI installed and on PATH?)\n";
    return 1;
  }
  if (std::find(available_models.begin(), available_models.end(), model) ==
      available_models.end()) {
    std::cerr << "error: model '" << model
              << "' is not available. Run 'ollama list' to see available "
                 "models.\n";
    return 1;
  }

  const std::string launch_dir = std::filesystem::current_path().string();

  const agent::YoloPolicy yolo_policy;
  const agent::SanePolicy sane_policy(launch_dir);
  const agent::PolicyInterface& policy =
      policy_name == "sane"
          ? static_cast<const agent::PolicyInterface&>(sane_policy)
          : static_cast<const agent::PolicyInterface&>(yolo_policy);

  agent::OllamaClient::configure(model);
  agent::AgentPool::configure(/*max_agents=*/16, /*max_depth=*/3);

  const agent::VenvBootstrap venv = agent::create_workspace_venv();
  if (venv.status == agent::VenvBootstrap::Status::Failed) {
    std::cerr << "error: could not create the .m8trixenv virtualenv at "
              << venv.venv_dir << "\n       " << venv.detail << "\n";
    return 1;
  }

  std::int64_t window = num_ctx;
  if (window <= 0) {
    window = agent::OllamaClient::instance().context_length(model);
  }
  if (window > 0) agent::OllamaClient::set_num_ctx(window);

  // --- shared UI state ---------------------------------------------------
  std::mutex ui_mutex;
  std::list<TranscriptNode> transcript;
  std::unordered_map<std::string, std::list<TranscriptNode>*> agent_containers;
  std::unordered_map<std::string, TranscriptNode*> subagent_nodes;
  std::vector<std::string> running_agents;

  Mode mode = Mode::Shell;
  bool mode_toggle_enabled = true;
  bool ai_turn_running = false;
  bool pending_prompt = false;
  bool left_unread = false;
  bool want_prompt_redraw = false;
  std::optional<std::promise<std::string>> answer_promise;
  float left_scroll = 1.0f;
  int left_width = 0;  // 0 => auto (2/5 of the terminal)
  f::Box left_viewport_box = kNoBox;
  std::int64_t ctx_tokens = 0;
  std::int64_t ctx_budget = 0;
  std::string last_osc_cwd;
  std::atomic<bool> shutting_down{false};

  std::mutex pty_mutex;
  std::string pty_pending;
  std::vector<std::string> submitted_lines;  // from the shell, in ai mode

  const std::string root_id = agent::AgentPool::instance().register_root("root");

  auto screen = f::App::Fullscreen();
  screen.ForceHandleCtrlC(false);
  screen.ForceHandleCtrlZ(false);

  // --- the shell integration ----------------------------------------------
  // A throwaway ZDOTDIR that runs the user's ~/.zshrc and then layers
  // m8trixsh's prompt, mode tag, and Enter-capture on top. zsh only; a
  // non-zsh shell still runs in the pane, just without the tag / capture.
  m8sh::PromptConfig prompt_config;
  prompt_config.format = settings.prompt_format.value_or("");
  prompt_config.shell_tag = settings.prompt_shell_tag.value_or("");
  prompt_config.ai_tag = settings.prompt_ai_tag.value_or("");
  prompt_config.ask_tag = settings.prompt_ask_tag.value_or("");
  m8sh::ShellIntegration integration(prompt_config);

  const std::string resolved_shell = agent::resolve_shell(shell_override);
  const bool zsh_shell = m8sh::shell_is_zsh(resolved_shell);
  std::vector<std::pair<std::string, std::string>> shell_env;
  if (zsh_shell and integration.ok()) {
    shell_env = integration.env();
  } else if (not zsh_shell) {
    std::cerr << "note: " << resolved_shell
              << " is not zsh; the [shell]/[m8trx] prompt tag and ai-mode "
                 "line capture are disabled (the shell pane still works)\n";
  } else {
    std::cerr << "warning: shell integration unavailable (" << integration.error()
              << "); the prompt tag and ai-mode line capture are disabled\n";
  }
  const bool integration_active = zsh_shell and integration.ok();

  // --- the terminal pane ----------------------------------------------------
  m8sh::TerminalEmulator emu(80, 24);
  agent::ShellSession shell;

  emu.on_pty_write = [&shell](std::string_view b) { shell.write_bytes(b); };
  emu.on_osc_cwd = [&](std::string p) {
    std::lock_guard<std::mutex> lock(ui_mutex);
    last_osc_cwd = std::move(p);
  };
  emu.on_line_submit = [&](std::string line) {
    std::lock_guard<std::mutex> lock(pty_mutex);
    submitted_lines.push_back(std::move(line));
  };
  shell.on_bytes = [&](std::string_view b) {
    if (shutting_down.load()) return;
    {
      std::lock_guard<std::mutex> lock(pty_mutex);
      pty_pending.append(b);
    }
    screen.PostEvent(f::Event::Custom);
  };
  shell.on_exit = [&](int) {
    if (shutting_down.load()) return;
    screen.Post([&] { screen.Exit(); });
  };

  if (not integration_active) mode_toggle_enabled = false;

  {
    std::string shell_error;
    if (not shell.start(80, 24, shell_override, &shell_error, shell_env)) {
      std::cerr << "error: could not start the shell: " << shell_error << "\n";
      return 1;
    }
  }

  // --- the agent ----------------------------------------------------------
  agent::AgentOptions options;
  options.max_steps = max_steps;
  options.context_window_tokens =
      static_cast<int>(std::max<std::int64_t>(0, window));
  options.context_summarize_at_tokens = summarize_at;
  options.skills_dir = skills_dir;
  options.enable_skills = not no_skills;
  options.enable_subagents = settings.enable_subagents.value_or(false);
  options.enable_package_install =
      settings.enable_package_install.value_or(true);
  options.enable_file_tools = true;
  options.enable_web_search = settings.enable_web_search.value_or(false);
  if (options.enable_web_search and not agent::web_search_available()) {
    std::cerr << "warning: ENABLE_WEB_SEARCH is set but no Parallel API key was "
                 "found (PARALLEL_API_KEY or .m8trix/parallel_api_key); "
                 "websearch calls will fail\n";
  }
  options.extra_system_prompt = workflow_prompt();
  options.ask_user_handler = [&](const std::string& prompt) -> std::string {
    std::future<std::string> answer;
    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      add_node(transcript, TranscriptNode::Kind::Notice, "\xe2\x97\x86 agent asks:");
      add_node(transcript, TranscriptNode::Kind::Assistant, prompt);
      answer_promise.emplace();
      answer = answer_promise->get_future();
      pending_prompt = true;
      left_scroll = 1.0f;
      if (mode == Mode::Shell) {
        left_unread = true;
      } else if (integration_active) {
        integration.set_mode("ai-ask");
        want_prompt_redraw = true;
      }
    }
    screen.PostEvent(f::Event::Custom);

    std::string reply = answer.get();  // parks this agent thread only
    if (shutting_down.load()) return reply;

    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      pending_prompt = false;
      answer_promise.reset();
      add_node(transcript, TranscriptNode::Kind::User, reply);
      left_scroll = 1.0f;
      if (integration_active and mode == Mode::Ai) {
        integration.set_mode("ai");
        want_prompt_redraw = true;
      }
    }
    screen.PostEvent(f::Event::Custom);
    return reply;
  };

  agent::Agent root_agent(options, policy, root_id, "", 0);
  agent_containers[root_id] = &transcript;

  // --- observer ---------------------------------------------------------
  agent::AgentPool::instance().set_observer([&](const agent::AgentEvent& event) {
    if (shutting_down.load()) return;
    {
      std::lock_guard<std::mutex> lock(ui_mutex);

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
          for (TranscriptNode& node : *container) {
            forget_subtree(node, subagent_nodes, agent_containers);
          }
          container->clear();
          if (event.depth == 0) {
            subagent_nodes.clear();
            agent_containers.clear();
            agent_containers[root_id] = &transcript;
            container = &transcript;
          }
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
      left_scroll = 1.0f;
      if (mode == Mode::Shell) left_unread = true;
    }
    screen.PostEvent(f::Event::Custom);
  });

  auto push_notice = [&](TranscriptNode::Kind kind, std::string text) {
    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      add_node(transcript, kind, std::move(text));
      left_scroll = 1.0f;
      if (mode == Mode::Shell) left_unread = true;
    }
    screen.PostEvent(f::Event::Custom);
  };

  auto start_turn = [&](std::string display, std::string objective) {
    std::string dir = shell.cwd();
    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      if (dir.empty()) dir = last_osc_cwd;
    }
    if (not dir.empty()) {
      std::error_code ec;
      std::filesystem::current_path(dir, ec);
    }

    std::list<TranscriptNode>::iterator turn_begin;
    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      add_node(transcript, TranscriptNode::Kind::User, std::move(display));
      turn_begin = std::prev(transcript.end());
      ai_turn_running = true;
      left_scroll = 1.0f;
    }
    screen.PostEvent(f::Event::Custom);

    std::thread([&root_agent, &ui_mutex, &transcript, &ai_turn_running,
                &left_scroll, &screen, &shutting_down,
                objective = std::move(objective), turn_begin] {
      const agent::AgentResult result = root_agent.run_turn(objective);
      if (shutting_down.load()) return;
      std::lock_guard<std::mutex> lock(ui_mutex);
      if (not result.ok and not result.hit_step_limit and
          not result.error.empty()) {
        add_node(transcript, TranscriptNode::Kind::Error, result.error);
      }
      for (auto it = turn_begin; it != transcript.end(); ++it) {
        collapse_subtree(*it);
      }
      ai_turn_running = false;
      left_scroll = 1.0f;
      screen.PostEvent(f::Event::Custom);
    }).detach();
  };

  // A line the user typed at the shell prompt while in ai mode (captured by the
  // integration and handed over via OSC 5171). Runs on the UI thread.
  auto handle_submitted_line = [&](const std::string& entered) {
    if (entered.empty()) return;

    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      if (pending_prompt and answer_promise.has_value()) {
        answer_promise->set_value(entered);
        return;
      }
    }

    if (entered == "/quit") {
      {
        std::lock_guard<std::mutex> lock(ui_mutex);
        if (answer_promise.has_value()) {
          answer_promise->set_value("");
          answer_promise.reset();
          pending_prompt = false;
        }
      }
      screen.Exit();
      return;
    }
    if (entered == "/help") {
      std::string help = kHelpText;
      for (const agent::SkillInfo& skill : root_agent.skill_catalog().skills) {
        if (not skill.command) continue;
        help += "\n/" + skill.name +
                (skill.argument_hint.empty() ? "" : "  " + skill.argument_hint) +
                " - " + skill.description;
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
      const std::int64_t used = root_agent.context_tokens();
      const std::int64_t win = root_agent.context_window();
      const std::int64_t limit = root_agent.context_limit();
      std::string msg = used > 0
                            ? "context: ~" + std::to_string(used) + " tokens"
                            : "context: not measured yet";
      if (win > 0) msg += "  (window " + std::to_string(win) + ")";
      msg += "  auto-summarize at " + std::to_string(limit);
      push_notice(TranscriptNode::Kind::Notice, msg);
      return;
    }
    if (entered == "/reset") {
      root_agent.reset();
      {
        std::lock_guard<std::mutex> lock(ui_mutex);
        transcript.clear();
        subagent_nodes.clear();
        agent_containers.clear();
        agent_containers[root_id] = &transcript;
        running_agents.clear();
      }
      push_notice(TranscriptNode::Kind::Notice, "started a new session");
      return;
    }
    if (entered == "/skills") {
      if (not ai_turn_running) root_agent.reload_skills();
      const agent::SkillCatalog& catalog = root_agent.skill_catalog();
      std::string msg;
      for (const agent::SkillInfo& skill : catalog.skills) {
        msg += (skill.command ? "/" : "  ") + skill.name + " - " +
               skill.description + "\n";
      }
      for (const std::string& note : catalog.notes) msg += "(" + note + ")\n";
      push_notice(TranscriptNode::Kind::Notice,
                  msg.empty() ? "no skills found in the skills directory" : msg);
      return;
    }

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
                                                        "\n" +
                                                        skill->description);
          return;
        }
        bool busy = false;
        {
          std::lock_guard<std::mutex> lock(ui_mutex);
          busy = ai_turn_running;
        }
        if (busy) {
          push_notice(TranscriptNode::Kind::Notice,
                      "the agent is working; wait for it to finish");
          return;
        }
        std::string objective =
            "Run the \"" + cmd +
            "\" skill: use the `skill` tool to load it, then follow its "
            "SKILL.md instructions.";
        if (not cmd_args.empty()) objective += "  Arguments: " + cmd_args;
        start_turn(entered, std::move(objective));
        return;
      }
    }

    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      if (ai_turn_running) {
        add_node(transcript, TranscriptNode::Kind::Notice,
                 "the agent is working; switch to the shell, or wait for it "
                 "to ask you");
        left_scroll = 1.0f;
        screen.PostEvent(f::Event::Custom);
        return;
      }
    }
    start_turn(entered, entered);
  };

  const auto is_switch_key = parse_switch_key(switch_key_name);

  auto on_resize = [&](int cols, int rows) {
    emu.set_size(cols, rows);
    shell.resize(cols, rows);
  };

  f::Box term_box = kNoBox;

  auto root = f::Renderer([&] {
    // Drain the pty into the emulator (UI thread; libvterm is single-threaded).
    // Feeding it can fire emu.on_line_submit, which queues into submitted_lines.
    {
      std::string chunk;
      std::vector<std::string> lines_in;
      {
        std::lock_guard<std::mutex> lock(pty_mutex);
        chunk.swap(pty_pending);
      }
      if (not chunk.empty()) emu.feed(chunk);
      {
        std::lock_guard<std::mutex> lock(pty_mutex);
        lines_in.swap(submitted_lines);
      }
      for (const std::string& line : lines_in) handle_submitted_line(line);
    }

    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      if (want_prompt_redraw) {
        want_prompt_redraw = false;
        emu.write_raw(m8sh::ShellIntegration::redraw_sequence());
      }
    }

    std::vector<f::Element> lines;
    bool expand_left = false;
    bool prompt_pending = false;
    bool unread = false;
    float scroll = 1.0f;
    int lwidth = 0;
    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      for (TranscriptNode& node : transcript) reset_boxes(node);
      expand_left =
          (mode == Mode::Ai) or ai_turn_running or pending_prompt;
      prompt_pending = pending_prompt;
      unread = left_unread;
      scroll = left_scroll;
      lwidth = left_width;
      if (expand_left) {
        for (TranscriptNode& node : transcript) render_node(lines, node, 0);
        if (ai_turn_running) {
          lines.push_back(f::text("agent is working...") | f::dim);
        }
      } else {
        left_viewport_box = kNoBox;
      }
    }

    const int auto_width =
        std::max(44, f::Terminal::Size().dimx * 2 / 5);
    const int pane_width = lwidth > 0 ? lwidth : auto_width;

    f::Element left;
    if (expand_left) {
      left = f::vbox(std::move(lines)) |
             f::focusPositionRelative(0.f, scroll) | f::vscroll_indicator |
             f::yframe | f::flex | f::reflect(left_viewport_box) |
             f::size(f::WIDTH, f::EQUAL, pane_width);
    } else {
      const char* dot = prompt_pending ? " \xe2\x97\x8f"
                                       : (unread ? " \xe2\x97\x8b" : "  ");
      left = f::vbox({
                 f::vtext(" AI ") | f::bold,
                 f::text(dot) | f::color(prompt_pending ? f::Color::Red
                                                       : f::Color::Blue),
             }) |
             f::border | f::size(f::WIDTH, f::EQUAL, 3);
    }

    // The shell always holds the keyboard, so it always draws the cursor.
    f::Element right =
        m8sh::terminal_element(emu, on_resize, &term_box, /*focused=*/true) |
        f::flex;

    return f::hbox({left, f::separator(), right}) | f::border;
  });

  root = f::CatchEvent(root, [&](f::Event event) {
    // The mode toggle: flip, tell the shell, repaint the prompt in place.
    if (mode_toggle_enabled and is_switch_key(event)) {
      std::lock_guard<std::mutex> lock(ui_mutex);
      mode = (mode == Mode::Shell) ? Mode::Ai : Mode::Shell;
      if (mode == Mode::Ai) {
        left_unread = false;
        integration.set_mode(pending_prompt ? "ai-ask" : "ai");
      } else {
        integration.set_mode("shell");
      }
      emu.write_raw(m8sh::ShellIntegration::redraw_sequence());
      screen.PostEvent(f::Event::Custom);
      return true;
    }

    // Ctrl-C / Ctrl-Z: straight to the foreground job, in either mode.
    if (event == f::Event::CtrlC) {
      emu.write_raw(std::string(1, '\x03'));
      return true;
    }
    if (event == f::Event::CtrlZ) {
      emu.write_raw(std::string(1, '\x1a'));
      return true;
    }

    // The mouse drives the transcript pane when it is over it, the terminal
    // otherwise - in both modes.
    if (event.is_mouse()) {
      const f::Mouse& mouse = event.mouse();
      {
        std::lock_guard<std::mutex> lock(ui_mutex);
        if (not left_viewport_box.IsEmpty() and
            left_viewport_box.Contain(mouse.x, mouse.y)) {
          if (mouse.button == f::Mouse::Left and
              mouse.motion == f::Mouse::Pressed) {
            for (TranscriptNode& node : transcript) {
              if (hit_test(node, mouse.x, mouse.y)) return true;
            }
          }
          if (mouse.button == f::Mouse::WheelUp) {
            left_scroll = std::clamp(left_scroll - 0.1f, 0.f, 1.f);
            return true;
          }
          if (mouse.button == f::Mouse::WheelDown) {
            left_scroll = std::clamp(left_scroll + 0.1f, 0.f, 1.f);
            return true;
          }
        }
      }
      if (mouse.button == f::Mouse::WheelUp) {
        emu.scroll_view(3);
        screen.PostEvent(f::Event::Custom);
        return true;
      }
      if (mouse.button == f::Mouse::WheelDown) {
        emu.scroll_view(-3);
        screen.PostEvent(f::Event::Custom);
        return true;
      }
      if (emu.mouse_reporting() and not term_box.IsEmpty() and
          term_box.Contain(mouse.x, mouse.y)) {
        const int row = mouse.y - term_box.y_min;
        const int col = mouse.x - term_box.x_min;
        int button = 0;
        if (mouse.button == f::Mouse::Left) button = 1;
        else if (mouse.button == f::Mouse::Middle) button = 2;
        else if (mouse.button == f::Mouse::Right) button = 3;
        const bool pressed = mouse.motion == f::Mouse::Pressed;
        const bool motion = mouse.motion == f::Mouse::Moved;
        emu.handle_mouse(row, col, button, pressed, motion, 0);
        return true;
      }
      return true;  // swallow the rest
    }

    // A few keys drive the transcript pane, but only in ai mode - in shell
    // mode every key (PageUp, Ctrl+Alt+*, Tab, ...) belongs to the shell.
    bool in_ai_mode;
    {
      std::lock_guard<std::mutex> lock(ui_mutex);
      in_ai_mode = mode == Mode::Ai;
    }
    if (in_ai_mode) {
      if (event == f::Event::PageUp) {
        std::lock_guard<std::mutex> lock(ui_mutex);
        left_scroll = std::clamp(left_scroll - 0.3f, 0.f, 1.f);
        return true;
      }
      if (event == f::Event::PageDown) {
        std::lock_guard<std::mutex> lock(ui_mutex);
        left_scroll = std::clamp(left_scroll + 0.3f, 0.f, 1.f);
        return true;
      }
      if (event == f::Event::CtrlAltH) {
        std::lock_guard<std::mutex> lock(ui_mutex);
        left_width = std::max(20, (left_width > 0 ? left_width : 48) - 4);
        return true;
      }
      if (event == f::Event::CtrlAltL) {
        std::lock_guard<std::mutex> lock(ui_mutex);
        left_width = (left_width > 0 ? left_width : 48) + 4;
        return true;
      }
      if (event == f::Event::CtrlAltF) {
        std::lock_guard<std::mutex> lock(ui_mutex);
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
    }

    // Internal / synthetic events are not keystrokes; don't feed them to the
    // shell (Event::Custom is posted on every pty read).
    if (event == f::Event::Custom or event.is_cursor_position() or
        event.is_cursor_shape() or event.IsTerminalNameVersion() or
        event.IsTerminalEmulator() or event.IsTerminalCapabilities()) {
      return false;
    }

    return emu.handle_key(event);
  });

  screen.Loop(root);

  shutting_down.store(true);
  {
    std::lock_guard<std::mutex> lock(ui_mutex);
    if (answer_promise.has_value()) {
      answer_promise->set_value("");
      answer_promise.reset();
    }
  }
  agent::AgentPool::instance().set_observer({});

  const std::string final_session = root_agent.session_id();
  if (not final_session.empty()) {
    std::cout << "session saved: " << agent::kAgentSessionDir << "/"
              << final_session << ".json\n";
  }
  return 0;
}
