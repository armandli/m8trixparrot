#include <cstdio>

#include <algorithm>
#include <fstream>
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

#include <core/basic_ollama_client.h>

namespace f = ftxui;

namespace {

struct DisplayMessage {
  std::string role;
  std::string content;
};

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

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"m8trixparrot chat_tui - terminal chat client for Ollama"};

  std::string model = "qwen3.8:27b-mlx";
  std::string history_file;

  app.add_option("model,-m,--model", model, "Ollama model to chat with")
      ->capture_default_str();
  app.add_option("history_file,-o,--history", history_file,
                 "Path to write the chat transcript to on exit");

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

  agent::BasicOllamaClient client;

  std::mutex mutex;
  std::vector<DisplayMessage> history;
  bool waiting_for_reply = false;
  float scroll_y = 1.0f;  // 0 = top of history, 1 = bottom (most recent).

  std::string input_value;
  int input_cursor = 0;

  // Input history is only ever touched from the main thread (unlike the
  // fields above, which the background reply thread also writes), so it
  // needs no mutex.
  std::vector<std::string> input_history;  // Most-recent-last; capped below.
  constexpr size_t kMaxInputHistory = 100;
  size_t history_index = 0;  // == input_history.size() means "viewing the live draft".
  std::string history_draft;  // Saved in-progress text while browsing history.

  auto screen = f::App::TerminalOutput();

  auto send_message = [&] {
    if (input_value.empty()) {
      return;
    }
    if (input_value == "/quit") {
      screen.ExitLoopClosure()();
      return;
    }

    std::vector<agent::ChatMessage> outgoing;
    {
      std::lock_guard<std::mutex> lock(mutex);
      history.push_back({"user", input_value});
      for (const auto& message : history) {
        outgoing.push_back({message.role, message.content});
      }
      waiting_for_reply = true;
      scroll_y = 1.0f;
    }
    input_history.push_back(input_value);
    if (input_history.size() > kMaxInputHistory) {
      input_history.erase(input_history.begin());
    }
    history_index = input_history.size();
    history_draft.clear();
    input_value.clear();
    input_cursor = 0;

    std::thread([&client, &mutex, &history, &waiting_for_reply, &scroll_y,
                &screen, model, outgoing = std::move(outgoing)] {
      const agent::ChatResult result = client.chat(model, outgoing);

      std::lock_guard<std::mutex> lock(mutex);
      if (result.ok) {
        history.push_back({"assistant", result.content});
      } else {
        history.push_back({"assistant", "[error] " + result.error});
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
      "history, /quit to exit";
  // The default transform inverts colors on focus, which would flip this
  // back to black-on-white since the input stays focused for the app's
  // whole lifetime (it's the only focusable component).
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
      for (const auto& message : history) {
        const bool is_user = message.role == "user";
        lines.push_back(f::hbox({
            f::text(is_user ? "you: " : "bot: ") | f::bold |
                f::color(is_user ? f::Color::Cyan : f::Color::Green),
            f::paragraph(message.content),
        }));
      }
      if (waiting_for_reply) {
        lines.push_back(f::text("bot is thinking...") | f::dim);
      }
      current_scroll_y = scroll_y;
    }

    return f::vbox({
               f::text("m8trixparrot chat  |  model: " + model) | f::bold |
                   f::center,
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

  if (not history_file.empty()) {
    std::ofstream out(history_file, std::ios::trunc);
    if (out) {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& message : history) {
        out << (message.role == "user" ? "you: " : "bot: ")
            << message.content << "\n\n";
      }
    }
  }

  return 0;
}
