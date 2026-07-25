#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/ollama_client.hpp"

using namespace ftxui;

namespace {

struct DisplayMessage {
  std::string role;
  std::string content;
};

}  // namespace

int main(int argc, char** argv) {
  const std::string model = argc > 1 ? argv[1] : "gemma4";

  agent::OllamaClient client;

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

  auto screen = App::TerminalOutput();

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
      const agent::ChatResult result = client.Chat(model, outgoing);

      std::lock_guard<std::mutex> lock(mutex);
      if (result.ok) {
        history.push_back({"assistant", result.content});
      } else {
        history.push_back({"assistant", "[error] " + result.error});
      }
      waiting_for_reply = false;
      scroll_y = 1.0f;
      screen.PostEvent(Event::Custom);
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

  InputOption input_option;
  input_option.content = &input_value;
  input_option.cursor_position = &input_cursor;
  input_option.placeholder =
      "Enter to send, Shift+Enter/Alt+Enter for newline, Up/Down for "
      "history, /quit to exit";
  // The default transform inverts colors on focus, which would flip this
  // back to black-on-white since the input stays focused for the app's
  // whole lifetime (it's the only focusable component).
  input_option.transform = [](InputState state) {
    Element element = std::move(state.element);
    if (state.is_placeholder) {
      element |= dim;
    }
    return element | color(Color::White) | bgcolor(Color::Black);
  };
  auto input = Input(input_option);

  auto root = Renderer(input, [&] {
    std::vector<Element> lines;
    float current_scroll_y;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& message : history) {
        const bool is_user = message.role == "user";
        lines.push_back(hbox({
            text(is_user ? "you: " : "bot: ") | bold |
                color(is_user ? Color::Cyan : Color::Green),
            paragraph(message.content),
        }));
      }
      if (waiting_for_reply) {
        lines.push_back(text("bot is thinking...") | dim);
      }
      current_scroll_y = scroll_y;
    }

    return vbox({
               text("m8trixparrot chat  |  model: " + model) | bold | center,
               separator(),
               vbox(lines) | focusPositionRelative(0.f, current_scroll_y) |
                   vscroll_indicator | yframe | flex,
               separator(),
               input->Render() | color(Color::White) | bgcolor(Color::Black) | border,
           }) |
           border;
  });

  root = CatchEvent(root, [&](Event event) {
    constexpr float kWheelStep = 0.1f;
    constexpr float kPageStep = 0.3f;
    static const Event kAltEnterCR = Event::Special("\x1b\r");
    static const Event kAltEnterLF = Event::Special("\x1b\n");
    static const Event kShiftEnterCsiU = Event::Special("\x1b[13;2u");
    static const Event kShiftEnterLegacy = Event::Special("\x1b[27;2;13~");

    if (event == Event::PageUp) {
      std::lock_guard<std::mutex> lock(mutex);
      scroll_y = std::clamp(scroll_y - kPageStep, 0.f, 1.f);
      return true;
    }
    if (event == Event::PageDown) {
      std::lock_guard<std::mutex> lock(mutex);
      scroll_y = std::clamp(scroll_y + kPageStep, 0.f, 1.f);
      return true;
    }
    if (event.is_mouse()) {
      if (event.mouse().button == Mouse::WheelUp) {
        std::lock_guard<std::mutex> lock(mutex);
        scroll_y = std::clamp(scroll_y - kWheelStep, 0.f, 1.f);
        return true;
      }
      if (event.mouse().button == Mouse::WheelDown) {
        std::lock_guard<std::mutex> lock(mutex);
        scroll_y = std::clamp(scroll_y + kWheelStep, 0.f, 1.f);
        return true;
      }
    }

    if (event == Event::Return) {
      send_message();
      return true;
    }
    if (event == kAltEnterCR || event == kAltEnterLF ||
        event == kShiftEnterCsiU || event == kShiftEnterLegacy) {
      input_value.insert(static_cast<size_t>(input_cursor), "\n");
      input_cursor += 1;
      return true;
    }
    if (event == Event::ArrowUp) {
      if (!cursor_on_first_line()) {
        return false;  // Let Input move the cursor up within the draft.
      }
      recall_previous();
      return true;
    }
    if (event == Event::ArrowDown) {
      if (!cursor_on_last_line()) {
        return false;  // Let Input move the cursor down within the draft.
      }
      recall_next();
      return true;
    }
    return false;
  });

  screen.Loop(root);
  return 0;
}
