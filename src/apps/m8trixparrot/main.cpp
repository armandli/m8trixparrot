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

#include <core/agent.h>
#include <core/agent_pool.h>
#include <core/sane_policy.h>
#include <core/policy.h>
#include <core/tools.h>

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
// conversation readable afterwards. A subagent is a nested block: its own
// activity renders under it, indented, and folds to one line when it finishes.
struct TranscriptNode {
  enum struct Kind { User, Assistant, ToolGroup, Error, Notice, Subagent };

  Kind kind = Kind::Assistant;
  std::string text;                   // Every kind except ToolGroup / Subagent.
  std::vector<ToolSegment> segments;  // ToolGroup only.

  // Subagent only.
  std::string agent_id;
  std::string objective;
  int depth = 0;
  bool done = false;
  bool ok = true;
  std::list<TranscriptNode> children;  // std::list: pointers stay valid on push.

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
    "/context  show context token usage and the auto-summarize threshold\n"
    "/reset    start a new session (memory is kept)\n"
    "/quit     exit\n"
    "\n"
    "click a > / v header to fold or unfold that tool call, group, or subagent\n"
    "Ctrl+T    fold or unfold everything at once";

TranscriptNode& add_node(std::list<TranscriptNode>& nodes,
                         TranscriptNode::Kind kind, std::string text) {
  TranscriptNode node;
  node.kind = kind;
  node.text = std::move(text);
  nodes.push_back(std::move(node));
  return nodes.back();
}

// Appends a tool segment, starting a new group when the previous node isn't
// one — an assistant message between calls means a new round of tool use.
ToolSegment& open_segment(std::list<TranscriptNode>& transcript) {
  if (transcript.empty() or
      transcript.back().kind != TranscriptNode::Kind::ToolGroup) {
    TranscriptNode group;
    group.kind = TranscriptNode::Kind::ToolGroup;
    transcript.push_back(std::move(group));
  }
  transcript.back().segments.push_back(ToolSegment{});
  return transcript.back().segments.back();
}

// --- recursive transcript helpers -------------------------------------------

void reset_boxes(TranscriptNode& node) {
  node.header_box = kNoBox;
  for (ToolSegment& segment : node.segments) segment.header_box = kNoBox;
  for (TranscriptNode& child : node.children) reset_boxes(child);
}

f::Element indent_line(int indent, f::Element element) {
  if (indent <= 0) return element;
  return f::hbox({f::text(std::string(2 * indent, ' ')), std::move(element)});
}

void render_node(std::vector<f::Element>& lines, TranscriptNode& node,
                 int indent) {
  switch (node.kind) {
    case TranscriptNode::Kind::User:
      lines.push_back(indent_line(
          indent, f::hbox({
                      f::text("you: ") | f::bold | f::color(f::Color::Cyan),
                      f::paragraph(node.text),
                  })));
      break;

    case TranscriptNode::Kind::Assistant:
      lines.push_back(indent_line(
          indent, f::hbox({
                      f::text("bot: ") | f::bold | f::color(f::Color::Green),
                      f::paragraph(node.text),
                  })));
      break;

    case TranscriptNode::Kind::Error:
      lines.push_back(indent_line(
          indent, f::hbox({
                      f::text("[error] ") | f::bold | f::color(f::Color::Red),
                      f::paragraph(node.text),
                  })));
      break;

    case TranscriptNode::Kind::Notice:
      lines.push_back(indent_line(indent, f::paragraph(node.text) | f::dim |
                                              f::color(f::Color::Magenta)));
      break;

    case TranscriptNode::Kind::ToolGroup: {
      const size_t count = node.segments.size();
      lines.push_back(indent_line(
          indent,
          f::hbox({
              f::text(node.expanded ? "v " : "> ") | f::bold |
                  f::color(f::Color::Yellow),
              f::text(std::to_string(count) +
                      (count == 1 ? " tool call" : " tool calls")) |
                  f::color(f::Color::Yellow),
              f::text(node.expanded ? "" : "  (click to expand)") | f::dim,
          }) |
              f::reflect(node.header_box)));

      if (not node.expanded) break;

      for (ToolSegment& segment : node.segments) {
        lines.push_back(indent_line(
            indent,
            f::hbox({
                f::text("  "),
                f::text(segment.expanded ? "v " : "> ") | f::bold |
                    f::color(segment.denied ? f::Color::Red : f::Color::Yellow),
                f::text(segment.tool_name + "  ") | f::bold |
                    f::color(segment.denied ? f::Color::Red : f::Color::Yellow),
                f::paragraph(segment.summary) | f::dim,
            }) |
                f::reflect(segment.header_box)));

        if (not segment.expanded or segment.result.empty()) continue;

        f::Element body =
            f::paragraph(clip_lines(segment.result, kMaxResultLines));
        body = segment.denied ? (body | f::color(f::Color::Red))
                              : (body | f::dim);
        lines.push_back(indent_line(indent, f::hbox({f::text("      "), body})));
      }
      break;
    }

    case TranscriptNode::Kind::Subagent: {
      const char* state = node.done ? (node.ok ? "  (done)" : "  (failed)")
                                    : "  (running)";
      const f::Color state_color =
          (node.done and not node.ok) ? f::Color::Red : f::Color::Blue;
      lines.push_back(indent_line(
          indent, f::hbox({
                      f::text(node.expanded ? "v " : "> ") | f::bold |
                          f::color(state_color),
                      f::text("subagent: ") | f::bold | f::color(state_color),
                      f::paragraph(clip_lines(node.objective, 1)) | f::dim,
                      f::text(state) | f::color(state_color),
                  }) |
                      f::reflect(node.header_box)));

      if (not node.expanded) break;
      for (TranscriptNode& child : node.children) {
        render_node(lines, child, indent + 1);
      }
      break;
    }
  }
}

// Toggles the header the click landed on, anywhere in the tree. Returns true
// when it consumed the click.
bool hit_test(TranscriptNode& node, int x, int y) {
  if (node.kind == TranscriptNode::Kind::ToolGroup) {
    if (node.header_box.Contain(x, y)) {
      node.expanded = not node.expanded;
      return true;
    }
    if (not node.expanded) return false;
    for (ToolSegment& segment : node.segments) {
      if (segment.header_box.Contain(x, y)) {
        segment.expanded = not segment.expanded;
        return true;
      }
    }
    return false;
  }
  if (node.kind == TranscriptNode::Kind::Subagent) {
    if (node.header_box.Contain(x, y)) {
      node.expanded = not node.expanded;
      return true;
    }
    if (not node.expanded) return false;
    for (TranscriptNode& child : node.children) {
      if (hit_test(child, x, y)) return true;
    }
    return false;
  }
  return false;
}

bool any_group_expanded(const TranscriptNode& node) {
  if ((node.kind == TranscriptNode::Kind::ToolGroup or
       node.kind == TranscriptNode::Kind::Subagent) and
      node.expanded) {
    return true;
  }
  for (const TranscriptNode& child : node.children) {
    if (any_group_expanded(child)) return true;
  }
  return false;
}

void set_all_expanded(TranscriptNode& node, bool expand) {
  if (node.kind == TranscriptNode::Kind::ToolGroup) {
    node.expanded = expand;
    for (ToolSegment& segment : node.segments) segment.expanded = expand;
  } else if (node.kind == TranscriptNode::Kind::Subagent) {
    node.expanded = expand;
  }
  for (TranscriptNode& child : node.children) set_all_expanded(child, expand);
}

// Folds a node and everything under it — used when a subagent finishes and when
// a turn ends.
void collapse_subtree(TranscriptNode& node) {
  if (node.kind == TranscriptNode::Kind::ToolGroup) {
    node.expanded = false;
    for (ToolSegment& segment : node.segments) segment.expanded = false;
  } else if (node.kind == TranscriptNode::Kind::Subagent) {
    node.expanded = false;
  }
  for (TranscriptNode& child : node.children) collapse_subtree(child);
}

// Renders a loaded result tree read-only: the conclusion, then a folded
// Subagent block for each child, recursively.
void render_result_body(std::list<TranscriptNode>& out,
                        const agent::AgentResult& result) {
  const std::string& text =
      result.ok ? result.conclusion
                : (result.error.empty() ? result.conclusion : result.error);
  add_node(out,
           result.ok ? TranscriptNode::Kind::Assistant
                     : TranscriptNode::Kind::Error,
           text);

  for (const agent::AgentResult& child : result.children) {
    TranscriptNode node;
    node.kind = TranscriptNode::Kind::Subagent;
    node.objective = child.objective;
    node.done = true;
    node.ok = child.ok;
    node.expanded = false;
    out.push_back(std::move(node));
    render_result_body(out.back().children, child);
  }
}

// "128k" / "1.5M" / "640".
std::string human_tokens(int64_t n) {
  if (n < 1000) return std::to_string(n);
  if (n < 1000000) return std::to_string((n + 500) / 1000) + "k";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1000000.0);
  return std::string(buf);
}

// Drops routing-map entries for `node` and every Subagent under it, before the
// list it lives in is cleared (a stale pointer into a cleared std::list would
// dangle). Safe because an agent that summarizes has already waited on its
// children, so their threads are done emitting.
void forget_subtree(
    TranscriptNode& node,
    std::unordered_map<std::string, TranscriptNode*>& subagent_nodes,
    std::unordered_map<std::string, std::list<TranscriptNode>*>&
        agent_containers) {
  for (TranscriptNode& child : node.children) {
    forget_subtree(child, subagent_nodes, agent_containers);
  }
  if (node.kind == TranscriptNode::Kind::Subagent and
      not node.agent_id.empty()) {
    subagent_nodes.erase(node.agent_id);
    agent_containers.erase(node.agent_id);
  }
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"m8trixparrot - a coding agent over Ollama"};

  std::string model = "qwen3.8:27b-mlx";
  std::string resume_id;
  bool resume_latest = false;
  int max_steps = 12;
  int max_depth = 3;
  int max_agents = 16;
  int num_ctx = 0;
  int summarize_at = 200000;

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
  // Bring Python up on the main thread before any agent thread touches it.
  agent::ensure_python_ready();

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

  auto screen = f::App::TerminalOutput();

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
          break;
        }

        case agent::AgentEvent::Kind::SubagentDone: {
          if (auto it = subagent_nodes.find(event.agent_id);
              it != subagent_nodes.end()) {
            it->second->done = true;
            it->second->ok = event.ok;
            collapse_subtree(*it->second);
          }
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
      const std::string notes = root_agent.memory();
      push_notice(TranscriptNode::Kind::Notice,
                  notes.empty() ? "no memory notes yet" : notes);
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
      }
      push_notice(TranscriptNode::Kind::Notice,
                  "started a new session; memory kept");
      return;
    }

    std::list<TranscriptNode>::iterator turn_begin;
    {
      std::lock_guard<std::mutex> lock(mutex);
      add_node(transcript, TranscriptNode::Kind::User, entered);
      turn_begin = std::prev(transcript.end());
      waiting_for_reply = true;
      scroll_y = 1.0f;
    }

    std::thread([&root_agent, &mutex, &transcript, &waiting_for_reply,
                 &scroll_y, &screen, entered, turn_begin] {
      const agent::AgentResult result = root_agent.run_turn(entered);

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
    int64_t header_ctx_tokens;
    int64_t header_ctx_budget;
    {
      std::lock_guard<std::mutex> lock(mutex);

      // Boxes are only meaningful for what this pass actually draws. Clearing
      // them first means a folded-away header can't be hit by a click landing
      // where it used to be.
      for (TranscriptNode& node : transcript) reset_boxes(node);

      for (TranscriptNode& node : transcript) render_node(lines, node, 0);

      if (waiting_for_reply) {
        lines.push_back(f::text("agent is working...") | f::dim);
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

    return f::vbox({
               f::text("m8trixparrot  |  model: " + model + "  |  policy: " +
                       policy.name() + ctx_part) |
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
        // beyond the frame still has a box, and it must not answer clicks.
        if (viewport_box.Contain(mouse.x, mouse.y)) {
          for (TranscriptNode& node : transcript) {
            if (hit_test(node, mouse.x, mouse.y)) return true;
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
