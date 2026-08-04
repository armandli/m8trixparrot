#include <cstdio>

#include <algorithm>
#include <iostream>
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

#include <core/basic_agent.hpp>
#include <core/policy.hpp>

namespace f = ftxui;

namespace {

// One rendered transcript entry. The agent reports events as they happen, so
// the screen holds display lines rather than the raw conversation.
struct DisplayLine {
  enum struct Kind { User, Assistant, ToolCall, ToolResult, Denied, Error, Notice };

  Kind kind = Kind::Assistant;
  std::string label;
  std::string text;
};

// A tool's output can be thousands of lines; the transcript shows the head of
// it and says how much it dropped.
constexpr size_t kMaxResultLines = 6;

std::string clip_lines(const std::string& text, size_t max_lines) {
  std::istringstream stream(text);
  std::string line;
  std::string clipped;
  size_t count = 0;

  while (std::getline(stream, line)) {
    if (count >= max_lines) {
      size_t remaining = 1;
      while (std::getline(stream, line)) ++remaining;
      clipped += "[... " + std::to_string(remaining) + " more lines]";
      break;
    }
    if (count > 0) clipped += "\n";
    clipped += line;
    ++count;
  }
  return clipped;
}

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
    "/memory   show the agent's memory notes\n"
    "/session  show the current session id\n"
    "/reset    start a new session (memory is kept)\n"
    "/quit     exit";

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"m8trixparrot mini_agent - a minimal coding agent over Ollama"};

  std::string model = "qwen2.5-coder:7b";
  std::string resume_id;
  bool resume_latest = false;
  int max_steps = 12;

  app.add_option("model,-m,--model", model, "Ollama model to run the agent on")
      ->capture_default_str();
  app.add_flag("-r,--resume", resume_latest,
               "Resume the most recent session in .mini_agent/sessions");
  app.add_option("-s,--session", resume_id, "Resume a specific session id");
  app.add_option("--max-steps", max_steps,
                 "Model calls allowed per user turn before giving up")
      ->capture_default_str();

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

  // Yolo for now: every tool call is permitted. Swapping in a stricter policy
  // is a change to this one line — BasicAgent only knows the interface.
  const agent::YoloPolicy policy;

  agent::AgentOptions options;
  options.model = model;
  options.max_steps = max_steps;

  agent::BasicAgent basic_agent(options, policy);

  std::mutex mutex;
  std::vector<DisplayLine> transcript;
  bool waiting_for_reply = false;
  float scroll_y = 1.0f;  // 0 = top of history, 1 = bottom (most recent).

  if (resume_latest or not resume_id.empty()) {
    const agent::SessionResult resumed = basic_agent.resume(resume_id);
    if (resumed.ok) {
      transcript.push_back({DisplayLine::Kind::Notice, "",
                            "resumed session " + resumed.session.session_id +
                                " (" +
                                std::to_string(resumed.session.interactions.size()) +
                                " messages)"});
      for (const auto& message : resumed.session.interactions) {
        if (message.role == "user") {
          transcript.push_back({DisplayLine::Kind::User, "you: ", message.content});
        } else if (message.role == "assistant" and not message.content.empty()) {
          transcript.push_back(
              {DisplayLine::Kind::Assistant, "bot: ", message.content});
        } else if (message.role == "tool") {
          transcript.push_back({DisplayLine::Kind::ToolResult,
                                "    " + message.tool_name + ": ",
                                clip_lines(message.content, kMaxResultLines)});
        }
      }
    } else {
      transcript.push_back(
          {DisplayLine::Kind::Error, "", "could not resume: " + resumed.error});
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

  auto screen = f::App::TerminalOutput();

  auto push_line = [&](DisplayLine line) {
    std::lock_guard<std::mutex> lock(mutex);
    transcript.push_back(std::move(line));
    scroll_y = 1.0f;
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

    if (entered == "/quit") {
      screen.ExitLoopClosure()();
      return;
    }
    if (entered == "/help") {
      push_line({DisplayLine::Kind::Notice, "", kHelpText});
      return;
    }
    if (entered == "/memory") {
      const std::string notes = basic_agent.memory();
      push_line({DisplayLine::Kind::Notice, "",
                 notes.empty() ? "no memory notes yet" : notes});
      return;
    }
    if (entered == "/session") {
      const std::string id = basic_agent.session_id();
      push_line({DisplayLine::Kind::Notice, "",
                 id.empty() ? "no session saved yet" : "session " + id});
      return;
    }
    if (entered == "/reset") {
      basic_agent.reset();
      {
        std::lock_guard<std::mutex> lock(mutex);
        transcript.clear();
      }
      push_line({DisplayLine::Kind::Notice, "",
                 "started a new session; memory kept"});
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex);
      transcript.push_back({DisplayLine::Kind::User, "you: ", entered});
      waiting_for_reply = true;
      scroll_y = 1.0f;
    }

    std::thread([&basic_agent, &mutex, &transcript, &waiting_for_reply,
                 &scroll_y, &screen, entered] {
      // Called from this worker thread as each step happens, which is what
      // makes the screen advance during a turn rather than after it.
      const agent::AgentObserver observer = [&](const agent::AgentEvent& event) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          switch (event.kind) {
            case agent::AgentEvent::Kind::Assistant:
              transcript.push_back(
                  {DisplayLine::Kind::Assistant, "bot: ", event.text});
              break;
            case agent::AgentEvent::Kind::ToolCall:
              transcript.push_back({DisplayLine::Kind::ToolCall,
                                    "  -> " + event.tool_name + "  ",
                                    event.summary});
              break;
            case agent::AgentEvent::Kind::ToolResult:
              transcript.push_back(
                  {DisplayLine::Kind::ToolResult, "     ",
                   clip_lines(event.text, kMaxResultLines)});
              break;
            case agent::AgentEvent::Kind::Denied:
              transcript.push_back({DisplayLine::Kind::Denied,
                                    "  denied " + event.tool_name + ": ",
                                    event.text});
              break;
            case agent::AgentEvent::Kind::Error:
              transcript.push_back(
                  {DisplayLine::Kind::Error, "[error] ", event.text});
              break;
            case agent::AgentEvent::Kind::Notice:
              transcript.push_back({DisplayLine::Kind::Notice, "", event.text});
              break;
          }
          scroll_y = 1.0f;
        }
        screen.PostEvent(f::Event::Custom);
      };

      const agent::AgentTurnResult result =
          basic_agent.run_turn(entered, observer);

      std::lock_guard<std::mutex> lock(mutex);
      if (not result.ok and not result.hit_step_limit and
          not result.error.empty()) {
        transcript.push_back(
            {DisplayLine::Kind::Error, "[error] ", result.error});
      }
      waiting_for_reply = false;
      scroll_y = 1.0f;
      screen.PostEvent(f::Event::Custom);
    }).detach();
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
      "Enter to send, Shift+Enter/Alt+Enter for newline, Up/Down for "
      "history, /help for commands";
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
    float current_scroll_y;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& line : transcript) {
        f::Color label_color = f::Color::Green;
        bool dim_body = false;
        switch (line.kind) {
          case DisplayLine::Kind::User: label_color = f::Color::Cyan; break;
          case DisplayLine::Kind::Assistant: label_color = f::Color::Green; break;
          case DisplayLine::Kind::ToolCall: label_color = f::Color::Yellow; break;
          case DisplayLine::Kind::ToolResult:
            label_color = f::Color::GrayDark;
            dim_body = true;
            break;
          case DisplayLine::Kind::Denied: label_color = f::Color::Red; break;
          case DisplayLine::Kind::Error: label_color = f::Color::Red; break;
          case DisplayLine::Kind::Notice:
            label_color = f::Color::Magenta;
            dim_body = true;
            break;
        }

        f::Element body = f::paragraph(line.text);
        if (dim_body) body = body | f::dim;

        if (line.label.empty()) {
          lines.push_back(body);
        } else {
          lines.push_back(f::hbox({
              f::text(line.label) | f::bold | f::color(label_color),
              body,
          }));
        }
      }
      if (waiting_for_reply) {
        lines.push_back(f::text("agent is working...") | f::dim);
      }
      current_scroll_y = scroll_y;
    }

    return f::vbox({
               f::text("m8trixparrot mini_agent  |  model: " + model +
                       "  |  policy: " + policy.name()) |
                   f::bold | f::center,
               f::separator(),
               f::vbox(lines) |
                   f::focusPositionRelative(0.f, current_scroll_y) |
                   f::vscroll_indicator | f::yframe | f::flex,
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
      if (event.mouse().button == f::Mouse::WheelUp) {
        std::lock_guard<std::mutex> lock(mutex);
        scroll_y = std::clamp(scroll_y - kWheelStep, 0.f, 1.f);
        return true;
      }
      if (event.mouse().button == f::Mouse::WheelDown) {
        std::lock_guard<std::mutex> lock(mutex);
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

  const std::string final_session = basic_agent.session_id();
  if (not final_session.empty()) {
    std::cout << "session saved: " << agent::kAgentSessionDir << "/"
              << final_session << ".json\n";
  }

  return 0;
}
