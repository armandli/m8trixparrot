#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

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
  const std::string model = argc > 1 ? argv[1] : "llama3.2";

  agent::OllamaClient client;

  std::mutex mutex;
  std::vector<DisplayMessage> history;
  bool waiting_for_reply = false;

  std::string input_value;

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
    }
    input_value.clear();

    std::thread([&client, &mutex, &history, &waiting_for_reply, &screen, model,
                 outgoing = std::move(outgoing)] {
      const agent::ChatResult result = client.Chat(model, outgoing);

      std::lock_guard<std::mutex> lock(mutex);
      if (result.ok) {
        history.push_back({"assistant", result.content});
      } else {
        history.push_back({"assistant", "[error] " + result.error});
      }
      waiting_for_reply = false;
      screen.PostEvent(Event::Custom);
    }).detach();
  };

  InputOption input_option;
  input_option.content = &input_value;
  input_option.placeholder = "Type a message, Enter to send, /quit to exit";
  input_option.multiline = false;  // Enter submits instead of inserting "\n".
  input_option.on_enter = send_message;
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
    }

    return vbox({
               text("m8trixparrot chat  |  model: " + model) | bold | center,
               separator(),
               vbox(lines) | flex,
               separator(),
               input->Render() | color(Color::White) | bgcolor(Color::Black) | border,
           }) |
           border;
  });

  screen.Loop(root);
  return 0;
}
