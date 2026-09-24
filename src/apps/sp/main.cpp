#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <list>
#include <mutex>
#include <optional>
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
#include <core/memory_store.h>
#include <core/policy.h>
#include <core/tools.h>

#include <sp_memory.h>

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

// sp's own home-directory config, in the shell-env format m8trixsh already
// uses for ~/.m8shrc (KEY=VALUE, `#` comments). sp reads
// .m8trix/settings.json too, but that path is relative to the working
// directory and sp is run from wherever the user happens to be standing — so
// the workspace file is almost never there, and this is the one that actually
// persists a setting.
inline constexpr const char* kSpRcFilename = ".sprc";

template <typename T>
void prefer(std::optional<T>& into, const std::optional<T>& over) {
  if (over.has_value()) into = over;
}

// The workspace file wins over the home file, being the more specific of the
// two. Only the fields sp reads are merged.
void merge_settings(agent::StartupSettings& into,
                    const agent::StartupSettings& over) {
  prefer(into.model, over.model);
  prefer(into.max_steps, over.max_steps);
  prefer(into.num_ctx, over.num_ctx);
  prefer(into.summarize_at, over.summarize_at);
  prefer(into.enable_memory, over.enable_memory);
  prefer(into.memory_path, over.memory_path);
  prefer(into.memory_embed_model, over.memory_embed_model);
}

const char* kHelpText =
    "/help             — show this message\n"
    "/reset            — clear context and start a new conversation\n"
    "/remember <text>  — store something about you or how you like things "
    "done\n"
    "/memories [query] — show what sp remembers, or search it\n"
    "/forget <id>      — delete one memory by id\n"
    "/quit             — exit shell-parrot\n"
    "\n"
    "Type a task in plain English and press Enter to run it.\n"
    "Shift+Enter or Alt+Enter inserts a newline.\n"
    "Up/Down arrows browse input history.";

// What memory is turned on, where it lives, and what embeds it — resolved once
// in main() and handed to whichever mode runs. `enabled` false means the agent
// has no `memory` tool, so the slash commands have nothing to talk to either.
struct MemoryConfig {
  bool enabled = false;
  agent::MemoryOptions options;
};

// The agent reaches its store through Agent::dispatch; the slash commands
// reach the same one through the registry, which keys on the canonical path —
// so both share a single open file rather than two stale views of it.
agent::MemoryStore* open_store(const MemoryConfig& memory, std::string& error) {
  return agent::MemoryStoreRegistry::instance().get(memory.options, error);
}

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

int run_interactive(agent::Agent& root_agent, const std::string& model,
                    const MemoryConfig& memory) {
  using namespace agentui;

  std::mutex mutex;
  std::list<TranscriptNode> transcript;
  bool waiting_for_reply = false;
  // A memory command holds a pointer into `transcript` until its embedding
  // call returns, so the transcript must not be cleared while one is in
  // flight. Guarded by `mutex`, like the transcript itself.
  int pending_memory_ops = 0;
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

  // A one-off line in the transcript, plus the input reset every command path
  // would otherwise repeat. Returns the node so a command that answers
  // asynchronously can fill it in later; std::list keeps the pointer valid.
  auto notice = [&](const std::string& text) -> TranscriptNode* {
    std::lock_guard<std::mutex> lock(mutex);
    TranscriptNode& node =
        add_node(transcript, TranscriptNode::Kind::Notice, text);
    input_value.clear();
    input_cursor = 0;
    scroll_y = 1.0f;
    return &node;
  };

  auto send_message = [&] {
    if (input_value.empty()) return;

    const sp::Command command = sp::parse_command(input_value);

    if (command.kind == sp::Command::Kind::Quit) {
      screen.ExitLoopClosure()();
      return;
    }

    if (command.kind == sp::Command::Kind::Help) {
      notice(kHelpText);
      return;
    }

    if (command.kind == sp::Command::Kind::Reset) {
      std::lock_guard<std::mutex> lock(mutex);
      if (waiting_for_reply or pending_memory_ops > 0) {
        add_node(transcript, TranscriptNode::Kind::Notice,
                 waiting_for_reply
                     ? "Cannot reset while a task is running."
                     : "Cannot reset while a memory command is running.");
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

    if (command.kind == sp::Command::Kind::BadForget) {
      notice("usage: /forget <id>   (ids come from /memories <query>)");
      return;
    }

    if (command.kind == sp::Command::Kind::Remember or
        command.kind == sp::Command::Kind::Memories or
        command.kind == sp::Command::Kind::Forget) {
      if (not memory.enabled) {
        notice("Memory is off. Pull an embedding model (ollama pull "
               "nomic-embed-text) or start sp with --memory.");
        return;
      }
      // remember and recall both block on an embedding round trip, so they
      // run off the UI thread for the same reason a turn does. The node goes
      // in now and is filled in when the answer arrives.
      TranscriptNode* node = notice("working...");
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++pending_memory_ops;
      }
      std::thread([&, command, node] {
        std::string error;
        agent::MemoryStore* store = open_store(memory, error);
        std::string text;
        if (store == nullptr) {
          text = "could not open the memory database: " + error;
        } else if (command.kind == sp::Command::Kind::Remember) {
          text = sp::do_remember(*store, command.args);
        } else if (command.kind == sp::Command::Kind::Memories) {
          text = sp::do_search(*store, command.args);
        } else {
          text = sp::do_forget(*store, command.id);
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          node->text = text;
          --pending_memory_ops;
          scroll_y = 1.0f;
        }
        screen.PostEvent(f::Event::Custom);
      }).detach();
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

  app.footer(
      "Defaults can be set in ~/.sprc (one KEY=VALUE per line; keys: MODEL, "
      "MAX_STEPS,\nNUM_CTX, SUMMARIZE_AT, ENABLE_MEMORY, MEMORY_PATH, "
      "MEMORY_EMBED_MODEL) and, per\ndirectory, in ./.m8trix/settings.json, "
      "which wins over ~/.sprc. A flag wins over both.");

  const std::string username = get_username();
  const std::string home     = get_home();

  std::string settings_warning;
  agent::StartupSettings settings = agent::load_shellrc_settings(
      home + "/" + kSpRcFilename, settings_warning);
  if (not settings_warning.empty()) {
    std::cerr << "warning: " << settings_warning << "\n";
    settings_warning.clear();
  }
  merge_settings(settings, agent::load_startup_settings(
                               agent::kAgentSettingsPath, settings_warning));
  if (not settings_warning.empty()) {
    std::cerr << "warning: " << settings_warning << "\n";
  }

  std::string model = settings.model.value_or("qwen3.8:27b-mlx");
  bool interactive = false;
  std::vector<std::string> prompt_words;
  int max_steps = settings.max_steps.value_or(30);
  int num_ctx = settings.num_ctx.value_or(0);

  std::string memory_path =
      settings.memory_path.value_or(sp::default_memory_path(home));
  std::string memory_model =
      settings.memory_embed_model.value_or("nomic-embed-text");
  // Unset means "decide from whether an embedding model is pulled"; a flag or
  // a config key makes it a decision the user made, which is honoured either
  // way.
  std::optional<bool> memory_wanted = settings.enable_memory;
  bool memory_flag = true;

  app.add_option("prompt", prompt_words, "Task description in plain English");
  app.add_flag("-i,--interactive", interactive,
               "Run in interactive TUI mode (default when no prompt is given)");
  app.add_option("-m,--model", model, "Ollama model to use")
      ->capture_default_str();
  app.add_option("--max-steps", max_steps,
                 "Max model calls per turn (default: 30)")
      ->capture_default_str();
  const CLI::Option* memory_opt = app.add_flag(
      "--memory,!--no-memory", memory_flag,
      "Force long-term memory on or off (default: on when an embedding model "
      "is pulled)");
  app.add_option("--memory-path", memory_path, "Memory database file")
      ->capture_default_str();
  app.add_option("--memory-model", memory_model, "Ollama embedding model")
      ->capture_default_str();

  CLI11_PARSE(app, argc, argv);

  if (memory_opt->count() > 0) memory_wanted = memory_flag;

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

  // Resolve memory last, because the probe is a call to the same Ollama the
  // model validation above has already shown to be up.
  MemoryConfig memory;
  memory.options.path = memory_path;
  memory.options.embed_model = memory_model;
  if (memory_wanted.value_or(true)) {
    const bool usable =
        agent::memory_available(memory_model, memory.options.ollama_host);
    if (usable) {
      memory.enabled = true;
    } else if (memory_wanted.has_value()) {
      // Asked for outright: honour it and say why the calls will fail, rather
      // than silently overruling the user. Same wording as m8trixparrot.
      memory.enabled = true;
      std::cerr << "warning: memory is enabled but '" << memory_model
                << "' is not a pulled embedding model (try `ollama pull "
                   "nomic-embed-text`); memory calls will fail\n";
    } else {
      // Nobody asked either way, so off is the safe read — one line to stderr
      // so a script's stdout stays clean.
      std::cerr << "note: long-term memory is off — `ollama pull "
                << memory_model << "` turns it on\n";
    }
  }

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
  options.enable_memory          = memory.enabled;
  options.memory_path            = memory.options.path;
  options.memory_embed_model     = memory.options.embed_model;
  options.extra_system_prompt    =
      sp::make_extra_prompt(username, home, memory.enabled);

  agent::AgentPool::configure(options.max_agents, options.max_depth);

  const agent::YoloPolicy policy;
  const std::string root_id =
      agent::AgentPool::instance().register_root("sp");
  agent::Agent root_agent(options, policy, root_id, "", 0);

  const int status = interactive ? run_interactive(root_agent, model, memory)
                                 : run_single_shot(prompt, root_agent);

  // Records reach the file as they are written, so nothing is lost without
  // this; flushing rewrites the header and search snapshot so the next process
  // opens without replaying the whole log. A store nothing ever touched
  // flushes to a no-op.
  if (memory.enabled) {
    std::string error;
    if (agent::MemoryStore* store = open_store(memory, error)) store->flush();
  }
  return status;
}
