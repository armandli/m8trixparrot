#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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
#include <core/tools.h>

namespace f = ftxui;

namespace {

std::vector<std::string> list_ollama_models(bool& command_ok) {
  std::vector<std::string> models;
  FILE* pipe = popen("ollama list 2>/dev/null", "r");
  if (pipe == nullptr) {
    command_ok = false;
    return models;
  }
  std::string output;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) output.append(buf, n);
  command_ok = (pclose(pipe) == 0);
  std::istringstream stream(output);
  std::string line;
  bool header = true;
  while (std::getline(stream, line)) {
    if (header) { header = false; continue; }
    std::istringstream ls(line);
    std::string name;
    if (ls >> name) models.push_back(name);
  }
  return models;
}

std::string get_username() {
  if (const char* u = std::getenv("USER")) return u;
  if (const char* u = std::getenv("LOGNAME")) return u;
  return "user";
}

std::string get_home() {
  if (const char* h = std::getenv("HOME")) return h;
  return "/tmp";
}

std::string make_extra_prompt(const std::string& username,
                              const std::string& home) {
  const std::string scripts = home + "/.local/share/sp/scripts";
  const std::string trash   = home + "/.local/share/Trash/files";
  std::ostringstream p;
  p << "You are shell-parrot (sp), a natural-language shell assistant. "
       "Your job is to carry out the user's task using shell commands.\n\n"
       "Shell environment:\n"
       "- User: " << username << "\n"
       "- Home: " << home << "\n"
       "- Scripts directory: " << scripts
    << " (create it first if it does not exist; write any helper scripts here)\n"
       "- Trash: " << trash
    << " — NEVER delete files; move them with 'mv <path> " << trash
    << "/' instead\n\n"
       "Rules:\n"
       "1. NEVER use rm, rmdir, unlink, or any deletion command. "
          "Move to trash instead.\n"
       "2. Use bash_search before complex tasks if you are unsure which "
          "commands are available.\n"
       "3. Write any helper scripts to the scripts directory, not to the "
          "working directory.\n"
       "4. When done, give a clear English summary of what you did and "
          "whether it succeeded.\n"
       "5. If a task is risky or irreversible, state what you are about "
          "to do before acting.\n";
  return p.str();
}

const char* kHelpText =
    "/help   — show this message\n"
    "/reset  — clear context and start a new conversation\n"
    "/quit   — exit shell-parrot\n"
    "\n"
    "Type a task in plain English and press Enter to run it.\n"
    "Shift+Enter or Alt+Enter inserts a newline.\n"
    "Up/Down arrows browse input history.";

}  // namespace

// ── single-shot mode ───────────────────────────────────────────────────────
//
// Runs one agent turn. Agent text responses are printed to stdout so a script
// can capture them; brief tool-call progress lines go to stderr.

int run_single_shot(const std::string& prompt, agent::Agent& root_agent) {
  agent::AgentPool::instance().set_observer([](const agent::AgentEvent& ev) {
    using K = agent::AgentEvent::Kind;
    switch (ev.kind) {
      case K::Assistant:
        std::cout << ev.text << "\n";
        std::cout.flush();
        break;
      case K::ToolCall:
        std::cerr << "[" << ev.tool_name;
        if (not ev.summary.empty()) std::cerr << ": " << ev.summary;
        std::cerr << "]\n";
        break;
      case K::Error:
        std::cerr << "error: " << ev.text << "\n";
        break;
      default:
        break;
    }
  });

  const agent::AgentResult result = root_agent.run_turn(prompt);
  return result.ok ? 0 : 1;
}

// ── interactive TUI mode ───────────────────────────────────────────────────

int run_interactive(agent::Agent& root_agent, const std::string& model) {
  using namespace agentui;

  std::mutex mutex;
  std::list<TranscriptNode> transcript;
  bool waiting_for_reply = false;
  float scroll_y = 1.0f;

  std::string input_value;
  int input_cursor = 0;
  std::vector<std::string> input_history;
  constexpr size_t kMaxInputHistory = 100;
  size_t history_index = 0;
  std::string history_draft;

  auto screen = f::App::Fullscreen();

  agent::AgentPool::instance().set_observer(
      [&](const agent::AgentEvent& ev) {
        using K = agent::AgentEvent::Kind;
        {
          std::lock_guard<std::mutex> lock(mutex);
          switch (ev.kind) {
            case K::Assistant:
              add_node(transcript, TranscriptNode::Kind::Assistant, ev.text);
              break;
            case K::ToolCall: {
              ToolSegment& seg = open_segment(transcript);
              seg.tool_name = ev.tool_name;
              seg.summary = ev.summary;
              break;
            }
            case K::ToolResult:
            case K::Denied:
              if (not transcript.empty() and
                  transcript.back().kind == TranscriptNode::Kind::ToolGroup and
                  not transcript.back().segments.empty()) {
                auto& seg = transcript.back().segments.back();
                seg.result = ev.text;
                seg.denied = (ev.kind == K::Denied);
              }
              break;
            case K::Error:
              add_node(transcript, TranscriptNode::Kind::Error, ev.text);
              break;
            case K::Notice:
              add_node(transcript, TranscriptNode::Kind::Notice, ev.text);
              break;
            case K::ContextSummarized:
              transcript.clear();
              add_node(transcript, TranscriptNode::Kind::Notice,
                       "Context compacted: " + ev.text);
              break;
            default:
              break;
          }
          scroll_y = 1.0f;
        }
        screen.PostEvent(f::Event::Custom);
      });

  auto send_message = [&] {
    if (input_value.empty()) return;

    if (input_value == "/quit") {
      screen.ExitLoopClosure()();
      return;
    }

    if (input_value == "/help") {
      std::lock_guard<std::mutex> lock(mutex);
      add_node(transcript, TranscriptNode::Kind::Notice, kHelpText);
      input_value.clear();
      input_cursor = 0;
      return;
    }

    if (input_value == "/reset") {
      std::lock_guard<std::mutex> lock(mutex);
      if (waiting_for_reply) {
        add_node(transcript, TranscriptNode::Kind::Notice,
                 "Cannot reset while a task is running.");
        input_value.clear();
        input_cursor = 0;
        return;
      }
      transcript.clear();
      root_agent.reset();
      add_node(transcript, TranscriptNode::Kind::Notice,
               "Conversation reset. Start a new task.");
      input_value.clear();
      input_cursor = 0;
      scroll_y = 1.0f;
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex);
      if (waiting_for_reply) return;
    }

    const std::string task = input_value;

    {
      std::lock_guard<std::mutex> lock(mutex);
      add_node(transcript, TranscriptNode::Kind::User, task);
      waiting_for_reply = true;
      scroll_y = 1.0f;
    }

    input_history.push_back(task);
    if (input_history.size() > kMaxInputHistory) {
      input_history.erase(input_history.begin());
    }
    history_index = input_history.size();
    history_draft.clear();
    input_value.clear();
    input_cursor = 0;

    std::thread([&root_agent, &mutex, &waiting_for_reply, &scroll_y,
                 &screen, task] {
      root_agent.run_turn(task);
      {
        std::lock_guard<std::mutex> lock(mutex);
        waiting_for_reply = false;
        scroll_y = 1.0f;
      }
      screen.PostEvent(f::Event::Custom);
    }).detach();
  };

  auto cursor_on_first_line = [&] {
    return input_value.substr(0, static_cast<size_t>(input_cursor))
               .find('\n') == std::string::npos;
  };
  auto cursor_on_last_line = [&] {
    return input_value.find('\n', static_cast<size_t>(input_cursor)) ==
           std::string::npos;
  };
  auto recall_previous = [&] {
    if (input_history.empty()) return;
    if (history_index == input_history.size()) history_draft = input_value;
    if (history_index == 0) return;
    --history_index;
    input_value = input_history[history_index];
    input_cursor = static_cast<int>(input_value.size());
  };
  auto recall_next = [&] {
    if (history_index >= input_history.size()) return;
    ++history_index;
    input_value = (history_index == input_history.size())
                      ? history_draft
                      : input_history[history_index];
    input_cursor = static_cast<int>(input_value.size());
  };

  f::InputOption input_opt;
  input_opt.content = &input_value;
  input_opt.cursor_position = &input_cursor;
  input_opt.placeholder =
      "Describe a task in English — Enter to run, Shift+Enter for newline, "
      "/help for commands";
  input_opt.transform = [](f::InputState state) {
    f::Element element = std::move(state.element);
    if (state.is_placeholder) element |= f::dim;
    return element | f::color(f::Color::White) | f::bgcolor(f::Color::Black);
  };
  auto input = f::Input(input_opt);

  auto root_component = f::Renderer(input, [&] {
    std::vector<f::Element> lines;
    float cur_scroll;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (auto& node : transcript) render_node(lines, node, 0);
      if (waiting_for_reply) {
        lines.push_back(f::text("sp is thinking...") | f::dim);
      }
      cur_scroll = scroll_y;
    }

    return f::vbox({
               f::hbox({f::text("shell-parrot (sp)") | f::bold,
                        f::text("  |  model: " + model) | f::dim}) |
                   f::center,
               f::separator(),
               f::vbox(std::move(lines)) |
                   f::focusPositionRelative(0.f, cur_scroll) |
                   f::vscroll_indicator | f::yframe | f::flex,
               f::separator(),
               input->Render() | f::color(f::Color::White) |
                   f::bgcolor(f::Color::Black) | f::border,
           }) |
           f::border;
  });

  root_component = f::CatchEvent(root_component, [&](f::Event event) {
    constexpr float kWheelStep = 0.1f;
    constexpr float kPageStep = 0.3f;
    static const f::Event kAltEnterCR = f::Event::Special("\x1b\r");
    static const f::Event kAltEnterLF = f::Event::Special("\x1b\n");
    static const f::Event kShiftEnterCsiU = f::Event::Special("\x1b[13;2u");
    static const f::Event kShiftEnterLeg = f::Event::Special("\x1b[27;2;13~");

    if (event == f::Event::PageUp) {
      std::lock_guard<std::mutex> lock(mutex);
      scroll_y = std::clamp(scroll_y - kPageStep, 0.f, 1.f);
      return true;
    }
    if (event == f::Event::PageDown) {
      std::lock_guard<std::mutex> lock(mutex);
      scroll_y = std::clamp(scroll_y + kPageStep, 0.f, 1.f);
      return true;
    }
    if (event.is_mouse()) {
      const bool up = event.mouse().button == f::Mouse::WheelUp;
      const bool down = event.mouse().button == f::Mouse::WheelDown;
      if (up or down) {
        std::lock_guard<std::mutex> lock(mutex);
        scroll_y = std::clamp(
            scroll_y + (down ? kWheelStep : -kWheelStep), 0.f, 1.f);
        return true;
      }
    }
    if (event == f::Event::Return) {
      send_message();
      return true;
    }
    if (event == kAltEnterCR or event == kAltEnterLF or
        event == kShiftEnterCsiU or event == kShiftEnterLeg) {
      input_value.insert(static_cast<size_t>(input_cursor), "\n");
      ++input_cursor;
      return true;
    }
    if (event == f::Event::ArrowUp) {
      if (not cursor_on_first_line()) return false;
      recall_previous();
      return true;
    }
    if (event == f::Event::ArrowDown) {
      if (not cursor_on_last_line()) return false;
      recall_next();
      return true;
    }
    return false;
  });

  screen.Loop(root_component);
  return 0;
}

// ── main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  CLI::App app{
      "shell-parrot (sp) — natural-language shell assistant\n"
      "Describe a task in plain English; sp runs the shell commands to carry "
      "it out.\n"
      "Files are never deleted — unwanted items are moved to the trash.\n"
      "\n"
      "Examples:\n"
      "  sp 'move all .log files in /tmp to ~/Downloads'\n"
      "  sp 'find the five largest files under ~/Documents'\n"
      "  sp 'rename every .jpeg in this directory to .jpg'\n"
      "  sp -i                   # interactive mode\n"
      "  sp                      # same — no prompt = interactive"};

  std::string settings_warning;
  const agent::StartupSettings settings =
      agent::load_startup_settings(agent::kAgentSettingsPath, settings_warning);
  if (not settings_warning.empty()) {
    std::cerr << "warning: " << settings_warning << "\n";
  }

  std::string model = settings.model.value_or("qwen3.8:27b-mlx");
  bool interactive = false;
  std::vector<std::string> prompt_words;
  int max_steps = settings.max_steps.value_or(30);
  int num_ctx = settings.num_ctx.value_or(0);

  app.add_option("prompt", prompt_words, "Task description in plain English");
  app.add_flag("-i,--interactive", interactive,
               "Run in interactive TUI mode (default when no prompt is given)");
  app.add_option("-m,--model", model, "Ollama model to use")
      ->capture_default_str();
  app.add_option("--max-steps", max_steps,
                 "Max model calls per turn (default: 30)")
      ->capture_default_str();

  CLI11_PARSE(app, argc, argv);

  std::string prompt;
  for (size_t i = 0; i < prompt_words.size(); ++i) {
    if (i > 0) prompt += ' ';
    prompt += prompt_words[i];
  }

  // No prompt → interactive mode. A prompt containing "interactive" → same.
  if (not interactive) {
    if (prompt.empty()) {
      interactive = true;
    } else {
      std::string lower = prompt;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (lower.find("interactive") != std::string::npos) {
        interactive = true;
      }
    }
  }

  bool list_ok = true;
  const std::vector<std::string> available_models =
      list_ollama_models(list_ok);
  if (not list_ok) {
    std::cerr
        << "error: failed to run 'ollama list' (is ollama on PATH?)\n";
    return 1;
  }
  if (std::find(available_models.begin(), available_models.end(), model) ==
      available_models.end()) {
    std::cerr << "error: model '" << model
              << "' is not available. Run 'ollama list' to see what is.\n";
    return 1;
  }

  agent::OllamaClient::configure(model);

  int64_t window = num_ctx;
  if (window <= 0) {
    window = agent::OllamaClient::instance().context_length(model);
    if (window <= 0 and not interactive) {
      std::cerr << "warning: could not detect context length for '" << model
                << "'; auto-summarizing at a flat " << 200000 << " tokens\n";
    }
  }
  if (window > 0) agent::OllamaClient::set_num_ctx(window);

  const std::string username = get_username();
  const std::string home     = get_home();

  agent::AgentOptions options;
  options.enable_python          = false;
  options.enable_subagents       = false;
  options.enable_skills          = false;
  options.enable_package_install = false;
  options.enable_file_tools      = false;
  options.enable_web_search      = false;
  options.enable_bash_search     = true;
  options.max_steps              = max_steps;
  options.max_depth              = 1;
  options.max_agents             = 1;
  options.context_window_tokens  =
      static_cast<int>(std::max<int64_t>(0, window));
  options.context_summarize_at_tokens =
      settings.summarize_at.value_or(200000);
  options.extra_system_prompt    = make_extra_prompt(username, home);

  agent::AgentPool::configure(options.max_agents, options.max_depth);

  const agent::YoloPolicy policy;
  const std::string root_id =
      agent::AgentPool::instance().register_root("sp");
  agent::Agent root_agent(options, policy, root_id, "", 0);

  if (interactive) {
    return run_interactive(root_agent, model);
  } else {
    return run_single_shot(prompt, root_agent);
  }
}
