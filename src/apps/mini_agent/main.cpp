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
#include <core/sane_policy.hpp>
#include <core/policy.hpp>

namespace f = ftxui;

namespace {

// A box no click can ever fall inside. Every header box is reset to this at
// the top of a render pass, so a header that isn't drawn this frame can't be
// hit by a click landing where it used to be.
constexpr f::Box kNoBox = {1, 0, 1, 0};

// One tool call and whatever it produced. Collapsing hides the result and
// leaves the header, which is the part that says what was run.
struct ToolSegment {
  std::string tool_name;
  std::string summary;  // The call's key arguments, on one line.
  std::string result;   // Tool output, or the policy's refusal.
  bool denied = false;
  bool expanded = true;  // Cleared when the turn finishes.
  f::Box header_box = kNoBox;
};

// One entry in the transcript. Tool activity is grouped rather than flat: a
// turn can make a dozen calls, and folding them to one line is what keeps the
// conversation readable afterwards.
struct TranscriptNode {
  enum struct Kind { User, Assistant, ToolGroup, Error, Notice };

  Kind kind = Kind::Assistant;
  std::string text;                   // Every kind except ToolGroup.
  std::vector<ToolSegment> segments;  // ToolGroup only.
  bool expanded = true;
  f::Box header_box = kNoBox;
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
    "/quit     exit\n"
    "\n"
    "click a > / v header to fold or unfold that tool call or group\n"
    "Ctrl+T    fold or unfold every tool group at once";

// Appends a tool segment, starting a new group when the previous node isn't
// one — an assistant message between calls means a new round of tool use.
ToolSegment& open_segment(std::vector<TranscriptNode>& transcript) {
  if (transcript.empty() or
      transcript.back().kind != TranscriptNode::Kind::ToolGroup) {
    TranscriptNode group;
    group.kind = TranscriptNode::Kind::ToolGroup;
    transcript.push_back(std::move(group));
  }
  transcript.back().segments.push_back(ToolSegment{});
  return transcript.back().segments.back();
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"m8trixparrot mini_agent - a minimal coding agent over Ollama"};

  std::string model = "gemma4:31b-mlx";
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

  std::string policy_name = "sane";
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

  // BasicAgent only knows PolicyInterface, so which policy is in force is
  // decided here and nowhere else.
  const agent::YoloPolicy yolo_policy;
  const agent::SanePolicy sane_policy;
  const agent::PolicyInterface& policy =
      policy_name == "sane"
          ? static_cast<const agent::PolicyInterface&>(sane_policy)
          : static_cast<const agent::PolicyInterface&>(yolo_policy);

  agent::AgentOptions options;
  options.model = model;
  options.max_steps = max_steps;

  agent::BasicAgent basic_agent(options, policy);

  std::mutex mutex;
  std::vector<TranscriptNode> transcript;
  bool waiting_for_reply = false;
  float scroll_y = 1.0f;  // 0 = top of history, 1 = bottom (most recent).
  f::Box viewport_box = kNoBox;

  const auto add_node = [](std::vector<TranscriptNode>& nodes,
                           TranscriptNode::Kind kind, std::string text) {
    TranscriptNode node;
    node.kind = kind;
    node.text = std::move(text);
    nodes.push_back(std::move(node));
  };

  if (resume_latest or not resume_id.empty()) {
    const agent::SessionResult resumed = basic_agent.resume(resume_id);
    if (resumed.ok) {
      add_node(transcript, TranscriptNode::Kind::Notice,
               "resumed session " + resumed.session.session_id + " (" +
                   std::to_string(resumed.session.interactions.size()) +
                   " messages)");
      for (const auto& message : resumed.session.interactions) {
        if (message.role == "user") {
          add_node(transcript, TranscriptNode::Kind::User, message.content);
        } else if (message.role == "assistant") {
          if (not message.content.empty()) {
            add_node(transcript, TranscriptNode::Kind::Assistant,
                     message.content);
          }
          // The assistant turn carries the calls; the results arrive as the
          // "tool" messages that follow, matched up in order below.
          for (const auto& call : message.tool_calls) {
            ToolSegment& segment = open_segment(transcript);
            segment.tool_name = call.name;
            segment.summary = clip_lines(call.arguments, 1);
            segment.expanded = false;
          }
          if (not transcript.empty() and
              transcript.back().kind == TranscriptNode::Kind::ToolGroup) {
            transcript.back().expanded = false;
          }
        } else if (message.role == "tool") {
          // Fill the first segment of the open group still awaiting a result.
          bool placed = false;
          for (auto node = transcript.rbegin();
               node != transcript.rend() and not placed; ++node) {
            if (node->kind != TranscriptNode::Kind::ToolGroup) break;
            for (ToolSegment& segment : node->segments) {
              if (segment.tool_name == message.tool_name and
                  segment.result.empty()) {
                segment.result = message.content;
                placed = true;
                break;
              }
            }
          }
          if (not placed) {
            ToolSegment& segment = open_segment(transcript);
            segment.tool_name = message.tool_name;
            segment.result = message.content;
            segment.expanded = false;
            transcript.back().expanded = false;
          }
        }
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

  auto screen = f::App::TerminalOutput();

  auto push_notice = [&](TranscriptNode::Kind kind, std::string text) {
    std::lock_guard<std::mutex> lock(mutex);
    add_node(transcript, kind, std::move(text));
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
      push_notice(TranscriptNode::Kind::Notice, kHelpText);
      return;
    }
    if (entered == "/memory") {
      const std::string notes = basic_agent.memory();
      push_notice(TranscriptNode::Kind::Notice,
                  notes.empty() ? "no memory notes yet" : notes);
      return;
    }
    if (entered == "/session") {
      const std::string id = basic_agent.session_id();
      push_notice(TranscriptNode::Kind::Notice,
                  id.empty() ? "no session saved yet" : "session " + id);
      return;
    }
    if (entered == "/reset") {
      basic_agent.reset();
      {
        std::lock_guard<std::mutex> lock(mutex);
        transcript.clear();
      }
      push_notice(TranscriptNode::Kind::Notice,
                  "started a new session; memory kept");
      return;
    }

    size_t turn_start = 0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      add_node(transcript, TranscriptNode::Kind::User, entered);
      turn_start = transcript.size();
      waiting_for_reply = true;
      scroll_y = 1.0f;
    }

    std::thread([&basic_agent, &mutex, &transcript, &waiting_for_reply,
                 &scroll_y, &screen, &add_node, entered, turn_start] {
      // Called from this worker thread as each step happens, which is what
      // makes the screen advance during a turn rather than after it. Segments
      // are created expanded so the run is visible live; they fold once the
      // turn is over.
      const agent::AgentObserver observer = [&](const agent::AgentEvent& event) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          switch (event.kind) {
            case agent::AgentEvent::Kind::Assistant:
              add_node(transcript, TranscriptNode::Kind::Assistant, event.text);
              break;
            case agent::AgentEvent::Kind::ToolCall: {
              ToolSegment& segment = open_segment(transcript);
              segment.tool_name = event.tool_name;
              segment.summary = event.summary;
              break;
            }
            case agent::AgentEvent::Kind::ToolResult:
            case agent::AgentEvent::Kind::Denied: {
              // Always answers the segment opened by the ToolCall just before.
              if (not transcript.empty() and
                  transcript.back().kind == TranscriptNode::Kind::ToolGroup and
                  not transcript.back().segments.empty()) {
                ToolSegment& segment = transcript.back().segments.back();
                segment.result = event.text;
                segment.denied =
                    (event.kind == agent::AgentEvent::Kind::Denied);
              }
              break;
            }
            case agent::AgentEvent::Kind::Error:
              add_node(transcript, TranscriptNode::Kind::Error, event.text);
              break;
            case agent::AgentEvent::Kind::Notice:
              add_node(transcript, TranscriptNode::Kind::Notice, event.text);
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
        add_node(transcript, TranscriptNode::Kind::Error, result.error);
      }
      // The turn is done, so its tool activity folds away and the transcript
      // reads as conversation again.
      for (size_t i = turn_start; i < transcript.size(); ++i) {
        if (transcript[i].kind != TranscriptNode::Kind::ToolGroup) continue;
        transcript[i].expanded = false;
        for (ToolSegment& segment : transcript[i].segments) {
          segment.expanded = false;
        }
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
    float current_scroll_y;
    {
      std::lock_guard<std::mutex> lock(mutex);

      // Boxes are only meaningful for what this pass actually draws. Clearing
      // them first means a folded-away header can't be hit by a click landing
      // where it used to be.
      for (TranscriptNode& node : transcript) {
        node.header_box = kNoBox;
        for (ToolSegment& segment : node.segments) {
          segment.header_box = kNoBox;
        }
      }

      for (TranscriptNode& node : transcript) {
        switch (node.kind) {
          case TranscriptNode::Kind::User:
            lines.push_back(f::hbox({
                f::text("you: ") | f::bold | f::color(f::Color::Cyan),
                f::paragraph(node.text),
            }));
            break;

          case TranscriptNode::Kind::Assistant:
            lines.push_back(f::hbox({
                f::text("bot: ") | f::bold | f::color(f::Color::Green),
                f::paragraph(node.text),
            }));
            break;

          case TranscriptNode::Kind::Error:
            lines.push_back(f::hbox({
                f::text("[error] ") | f::bold | f::color(f::Color::Red),
                f::paragraph(node.text),
            }));
            break;

          case TranscriptNode::Kind::Notice:
            lines.push_back(f::paragraph(node.text) | f::dim |
                            f::color(f::Color::Magenta));
            break;

          case TranscriptNode::Kind::ToolGroup: {
            const size_t count = node.segments.size();
            lines.push_back(
                f::hbox({
                    f::text(node.expanded ? "v " : "> ") | f::bold |
                        f::color(f::Color::Yellow),
                    f::text(std::to_string(count) +
                            (count == 1 ? " tool call" : " tool calls")) |
                        f::color(f::Color::Yellow),
                    f::text(node.expanded ? "" : "  (click to expand)") | f::dim,
                }) |
                f::reflect(node.header_box));

            if (not node.expanded) break;

            for (ToolSegment& segment : node.segments) {
              lines.push_back(
                  f::hbox({
                      f::text("  ") ,
                      f::text(segment.expanded ? "v " : "> ") | f::bold |
                          f::color(segment.denied ? f::Color::Red
                                                  : f::Color::Yellow),
                      f::text(segment.tool_name + "  ") | f::bold |
                          f::color(segment.denied ? f::Color::Red
                                                  : f::Color::Yellow),
                      f::paragraph(segment.summary) | f::dim,
                  }) |
                  f::reflect(segment.header_box));

              if (not segment.expanded or segment.result.empty()) continue;

              f::Element body =
                  f::paragraph(clip_lines(segment.result, kMaxResultLines));
              body = segment.denied ? (body | f::color(f::Color::Red))
                                    : (body | f::dim);
              lines.push_back(f::hbox({f::text("      "), body}));
            }
            break;
          }
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
                   f::vscroll_indicator | f::yframe | f::flex |
                   f::reflect(viewport_box),
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
    if (event == f::Event::CtrlT) {
      std::lock_guard<std::mutex> lock(mutex);
      // One toggle, not a per-node inversion: whatever the first group is
      // doing, everything follows the opposite.
      bool any_expanded = false;
      for (const TranscriptNode& node : transcript) {
        if (node.kind == TranscriptNode::Kind::ToolGroup and node.expanded) {
          any_expanded = true;
          break;
        }
      }
      const bool expand = not any_expanded;
      for (TranscriptNode& node : transcript) {
        if (node.kind != TranscriptNode::Kind::ToolGroup) continue;
        node.expanded = expand;
        for (ToolSegment& segment : node.segments) {
          segment.expanded = expand;
        }
      }
      return true;
    }

    if (event.is_mouse()) {
      const f::Mouse& mouse = event.mouse();
      if (mouse.button == f::Mouse::Left and
          mouse.motion == f::Mouse::Pressed) {
        std::lock_guard<std::mutex> lock(mutex);
        // Only headers inside the scrolling viewport are live: a row laid out
        // beyond the frame still has a box, and it must not answer clicks.
        if (viewport_box.Contain(mouse.x, mouse.y)) {
          for (TranscriptNode& node : transcript) {
            if (node.kind != TranscriptNode::Kind::ToolGroup) continue;
            if (node.header_box.Contain(mouse.x, mouse.y)) {
              node.expanded = not node.expanded;
              return true;
            }
            if (not node.expanded) continue;
            for (ToolSegment& segment : node.segments) {
              if (segment.header_box.Contain(mouse.x, mouse.y)) {
                segment.expanded = not segment.expanded;
                return true;
              }
            }
          }
        }
        // Fall through: a click elsewhere still belongs to the input.
      }
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
