#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iterator>
#include <list>
#include <mutex>
#include <optional>
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
#include <core/vdb/memory_store.h>
#include <core/policy/policy.h>
#include <core/tools/tools.h>

#include <sp_memory.h>
#include <sp_prompt.h>
#include <sp_paths.h>

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
  prefer(into.ollama_jobs, over.ollama_jobs);
  prefer(into.enable_subagents, over.enable_subagents);
  prefer(into.max_depth, over.max_depth);
  prefer(into.max_agents, over.max_agents);
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
    "Up/Down arrows browse input history.\n"
    "\n"
    "Ctrl+G  — while subagents run: swap the pane grid for the transcript\n"
    "Ctrl+T  — fold or unfold every tool and subagent block\n"
    "Click a folded header to open just that one.";

// What memory is turned on, where it lives, and what embeds it — resolved once
// in main() and handed to whichever mode runs. `enabled` false means the agent
// has no `memory` tool, so the slash commands have nothing to talk to either.
struct MemoryConfig {
  bool enabled = false;
  vdb::MemoryOptions options;
};

// The agent reaches its store through Agent::dispatch; the slash commands
// reach the same one through the registry, which keys on the canonical path —
// so both share a single open file rather than two stale views of it.
vdb::MemoryStore* open_store(const MemoryConfig& memory, std::string& error) {
  return vdb::MemoryStoreRegistry::instance().get(memory.options, error);
}

}  // namespace

// ── single-shot mode ───────────────────────────────────────────────────────
//
// Runs one agent turn. Agent text responses are printed to stdout so a script
// can capture them; brief tool-call progress lines go to stderr.

int run_single_shot(const std::string& prompt, agent::Agent& root_agent) {
  agent::AgentPool::instance().set_observer([](const agent::AgentEvent& ev) {
    using K = agent::AgentEvent::Kind;
    // AgentPool::emit holds its observer mutex for the whole call, so events
    // from every agent thread arrive one at a time and these writes need no
    // lock of their own. Indented by depth so an interleaved line says which
    // agent produced it.
    const std::string pad(2 * static_cast<size_t>(ev.depth), ' ');
    switch (ev.kind) {
      case K::Assistant:
        // Only the root's text is the answer a script is piping. A subagent's
        // final message is progress, and progress belongs on stderr.
        if (ev.depth == 0) {
          std::cout << ev.text << "\n";
          std::cout.flush();
        } else {
          std::cerr << pad << "[d" << ev.depth << " says: "
                    << agentui::clip_lines(ev.text, 1) << "]\n";
        }
        break;
      case K::ToolCall:
        std::cerr << pad << "[";
        if (ev.depth > 0) std::cerr << "d" << ev.depth << " ";
        std::cerr << ev.tool_name;
        if (not ev.summary.empty()) std::cerr << ": " << ev.summary;
        std::cerr << "]\n";
        break;
      case K::SubagentStart:
        // Stamped for the child, so indent one level shallower — the line
        // belongs to the parent that spawned it.
        std::cerr << std::string(2 * static_cast<size_t>(ev.depth - 1), ' ')
                  << "[subagent " << ev.agent_id.substr(0, 8) << " started: "
                  << agentui::clip_lines(ev.summary, 1) << "]\n";
        break;
      case K::SubagentDone:
        std::cerr << std::string(2 * static_cast<size_t>(ev.depth - 1), ' ')
                  << "[subagent " << ev.agent_id.substr(0, 8) << " "
                  << (ev.ok ? "done" : "failed") << ": " << ev.summary << "]\n";
        break;
      case K::Error:
        std::cerr << pad << "error: " << ev.text << "\n";
        break;
      default:
        break;
    }
  });

  const agent::AgentResult result = root_agent.run_turn(prompt);
  // run_turn has already gone through save() -> assemble_tree(), so every
  // descendant has published its result; dropping the observer anyway means the
  // return does not depend on that argument holding.
  agent::AgentPool::instance().set_observer({});
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

  // Where each agent's events land: the root into `transcript`, a subagent into
  // its own Subagent node's `children`. Without this every agent's output would
  // pile into one flat column. Guarded by `mutex`, like everything below.
  const std::string root_id = root_agent.id();
  std::unordered_map<std::string, std::list<TranscriptNode>*> agent_containers;
  std::unordered_map<std::string, TranscriptNode*> subagent_nodes;
  agent_containers[root_id] = &transcript;

  // Subagent ids currently running, in spawn order — an unordered_map's order
  // is unstable and would make panes jump between frames. Ids rather than
  // TranscriptNode*, because a summarize can clear the owning std::list: going
  // through `subagent_nodes` at render time lets a pruned pane simply vanish.
  std::vector<std::string> running_agents;
  // Ctrl+G while subagents run: show the transcript instead of the pane grid.
  bool force_conversation_view = false;
  int64_t ctx_tokens = 0;  // Root context usage, from ContextUsage events.
  int64_t ctx_budget = 0;  // The auto-summarize threshold.
  f::Box viewport_box = kNoBox;
  // Read by the observer on arbitrary agent threads after the UI has gone.
  std::atomic<bool> shutting_down{false};
  // Detached threads that still touch this frame. Decremented as each one's
  // very last act — after its final PostEvent, not before, because `screen` is
  // a local too and /quit must not destroy it out from under that call.
  // waiting_for_reply and pending_memory_ops both clear a moment too early to
  // serve as the drain condition.
  std::atomic<int> threads_in_flight{0};

  std::string input_value;
  int input_cursor = 0;
  std::vector<std::string> input_history;
  constexpr size_t kMaxInputHistory = 100;
  size_t history_index = 0;
  std::string history_draft;

  auto screen = f::App::Fullscreen();

  // One process-wide observer for every agent in the tree. Set before any turn
  // runs; the callback fires on arbitrary agent threads.
  agent::AgentPool::instance().set_observer(
      [&](const agent::AgentEvent& ev) {
        if (shutting_down.load()) return;
        using K = agent::AgentEvent::Kind;
        {
          std::lock_guard<std::mutex> lock(mutex);

          std::list<TranscriptNode>* container = &transcript;
          if (auto it = agent_containers.find(ev.agent_id);
              it != agent_containers.end()) {
            container = it->second;
          }

          switch (ev.kind) {
            case K::Assistant:
              add_node(*container, TranscriptNode::Kind::Assistant, ev.text);
              break;
            case K::ToolCall: {
              ToolSegment& seg = open_segment(*container);
              seg.tool_name = ev.tool_name;
              seg.summary = ev.summary;
              break;
            }
            case K::ToolResult:
            case K::Denied:
              if (not container->empty() and
                  container->back().kind == TranscriptNode::Kind::ToolGroup and
                  not container->back().segments.empty()) {
                auto& seg = container->back().segments.back();
                seg.result = ev.text;
                seg.denied = (ev.kind == K::Denied);
              }
              break;
            case K::Error:
              add_node(*container, TranscriptNode::Kind::Error, ev.text);
              break;
            case K::Notice:
              add_node(*container, TranscriptNode::Kind::Notice, ev.text);
              break;

            case K::SubagentStart: {
              // Stamped for the child, so the block belongs in the parent's
              // container; the child's own events then land inside it.
              std::list<TranscriptNode>* parent = &transcript;
              if (auto it = agent_containers.find(ev.parent_id);
                  it != agent_containers.end()) {
                parent = it->second;
              }
              TranscriptNode node;
              node.kind = TranscriptNode::Kind::Subagent;
              node.agent_id = ev.agent_id;
              node.objective = ev.summary;
              node.depth = ev.depth;
              node.expanded = true;
              parent->push_back(std::move(node));
              TranscriptNode& stored = parent->back();
              agent_containers[ev.agent_id] = &stored.children;
              subagent_nodes[ev.agent_id] = &stored;
              running_agents.push_back(ev.agent_id);
              break;
            }

            case K::SubagentDone: {
              if (auto it = subagent_nodes.find(ev.agent_id);
                  it != subagent_nodes.end()) {
                it->second->done = true;
                it->second->ok = ev.ok;
                collapse_subtree(*it->second);
              }
              std::erase(running_agents, ev.agent_id);
              break;
            }

            case K::ContextUsage:
              // A subagent's usage must not overwrite the root's header.
              if (ev.depth == 0) {
                ctx_tokens = ev.tokens;
                ctx_budget = ev.token_budget;
              }
              break;

            case K::ContextSummarized: {
              // Compact this agent's view to match its now-summarized
              // transcript. forget_subtree first: clearing the list without
              // dropping the routing entries would leave a dangling
              // TranscriptNode* for every subagent inside it.
              for (TranscriptNode& node : *container) {
                forget_subtree(node, subagent_nodes, agent_containers);
              }
              container->clear();
              if (ev.depth == 0) {
                // Everything lived under the root; rebuild the routing tables.
                subagent_nodes.clear();
                agent_containers.clear();
                agent_containers[root_id] = &transcript;
                container = &transcript;
              }
              // A subagent whose node was just pruned (or that already
              // finished) is no longer a live pane.
              std::erase_if(running_agents, [&](const std::string& id) {
                auto it = subagent_nodes.find(id);
                return it == subagent_nodes.end() or it->second->done;
              });
              add_node(*container, TranscriptNode::Kind::Notice,
                       "Context compacted: " + ev.text);
              break;
            }
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
      // Finished subagents from earlier turns still have entries pointing into
      // `transcript`; clearing it without dropping them would dangle. The
      // refusal above is what makes this safe for *running* subagents:
      // waiting_for_reply stays true until run_turn returns, and run_turn goes
      // through save() -> assemble_tree(), which cannot return while any
      // descendant is alive.
      for (TranscriptNode& node : transcript) {
        forget_subtree(node, subagent_nodes, agent_containers);
      }
      transcript.clear();
      subagent_nodes.clear();
      agent_containers.clear();
      agent_containers[root_id] = &transcript;
      running_agents.clear();
      force_conversation_view = false;
      ctx_tokens = 0;
      ctx_budget = 0;
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
        notice(std::string("Memory is off. Pull an embedding model (ollama "
                           "pull ") +
               oc::kDefaultEmbedModel + ") or start sp with --memory.");
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
      threads_in_flight.fetch_add(1);
      std::thread([&, command, node] {
        std::string error;
        vdb::MemoryStore* store = open_store(memory, error);
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
        threads_in_flight.fetch_sub(1);
      }).detach();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex);
      if (waiting_for_reply) return;
    }

    const std::string task = input_value;

    // Everything from here to the end of the transcript is this turn's, and
    // folds away when it finishes. std::list keeps the iterator valid.
    std::list<TranscriptNode>::iterator turn_begin;
    {
      std::lock_guard<std::mutex> lock(mutex);
      add_node(transcript, TranscriptNode::Kind::User, task);
      turn_begin = std::prev(transcript.end());
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

    threads_in_flight.fetch_add(1);
    std::thread([&root_agent, &mutex, &transcript, &waiting_for_reply,
                 &running_agents, &force_conversation_view, &scroll_y, &screen,
                 &threads_in_flight, task, turn_begin] {
      root_agent.run_turn(task);
      {
        std::lock_guard<std::mutex> lock(mutex);
        // The turn is done, so its tool activity and subagent blocks fold away
        // and the transcript reads as conversation again.
        for (auto it = turn_begin; it != transcript.end(); ++it) {
          collapse_subtree(*it);
        }
        // run_turn cannot return while a descendant is alive (save() ->
        // assemble_tree waits on each), so every subagent has emitted
        // SubagentDone by now; this is belt-and-braces so a missed event can't
        // strand the grid over the transcript.
        running_agents.clear();
        force_conversation_view = false;
        waiting_for_reply = false;
        scroll_y = 1.0f;
      }
      screen.PostEvent(f::Event::Custom);
      threads_in_flight.fetch_sub(1);
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

  const int max_agents = agent::AgentPool::instance().max_agents();

  auto root_component = f::Renderer(input, [&] {
    std::vector<f::Element> lines;
    std::vector<f::Element> tiles;
    float cur_scroll;
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
          if (it != subagent_nodes.end()) {
            tiles.push_back(render_pane(*it->second));
          }
        }
      } else {
        for (auto& node : transcript) render_node(lines, node, 0);
        if (waiting_for_reply) {
          // Naming the running subagents matters: the root can finish speaking
          // while save() still waits on a child, and a bare "thinking..." would
          // make that look like a hang.
          lines.push_back(
              f::text(running_n > 0
                          ? "sp is thinking... (" + std::to_string(running_n) +
                                (running_n == 1 ? " subagent" : " subagents") +
                                " running — Ctrl+G for the panes)"
                          : "sp is thinking...") |
              f::dim);
        }
      }
      cur_scroll = scroll_y;
      header_ctx_tokens = ctx_tokens;
      header_ctx_budget = ctx_budget;
    }

    std::string status = "  |  model: " + model;
    if (header_ctx_budget > 0) {
      status += "  |  ctx: " + human_tokens(header_ctx_tokens) + "/" +
                human_tokens(header_ctx_budget);
    }
    if (running_n > 0) {
      status += "  |  subagents: " + std::to_string(running_n) + "/" +
                std::to_string(max_agents) +
                (transcript_forced ? "  (Ctrl+G: grid)"
                                   : "  (Ctrl+G: transcript)");
    }

    f::Element middle;
    if (show_grid) {
      const int shown = static_cast<int>(tiles.size());
      const GridShape gs = grid_shape(std::max(shown, 1));
      std::vector<f::Elements> matrix;
      matrix.reserve(static_cast<size_t>(gs.rows));
      for (int r = 0; r < gs.rows; ++r) {
        f::Elements row;
        row.reserve(static_cast<size_t>(gs.cols));
        for (int c = 0; c < gs.cols; ++c) {
          const int idx = r * gs.cols + c;  // row-major fill
          row.push_back(idx < shown ? std::move(tiles[static_cast<size_t>(idx)])
                                    : empty_pane());
        }
        matrix.push_back(std::move(row));
      }
      middle = f::gridbox(std::move(matrix)) | f::flex;
    } else {
      middle = f::vbox(std::move(lines)) |
               f::focusPositionRelative(0.f, cur_scroll) |
               f::vscroll_indicator | f::yframe | f::flex |
               f::reflect(viewport_box);
    }

    return f::vbox({
               f::hbox({f::text("shell-parrot (sp)") | f::bold,
                        f::text(status) | f::dim}) |
                   f::center,
               f::separator(),
               std::move(middle),
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
    if (event == f::Event::PageUp) {
      std::lock_guard<std::mutex> lock(mutex);
      if (grid_active()) return true;  // The panes scroll themselves.
      scroll_y = std::clamp(scroll_y - kPageStep, 0.f, 1.f);
      return true;
    }
    if (event == f::Event::PageDown) {
      std::lock_guard<std::mutex> lock(mutex);
      if (grid_active()) return true;
      scroll_y = std::clamp(scroll_y + kPageStep, 0.f, 1.f);
      return true;
    }
    if (event.is_mouse()) {
      const f::Mouse& mouse = event.mouse();
      if (mouse.button == f::Mouse::Left and
          mouse.motion == f::Mouse::Pressed) {
        std::lock_guard<std::mutex> lock(mutex);
        // Only headers inside the scrolling viewport are live: a row laid out
        // beyond the frame still has a box and must not answer clicks. A
        // finished subagent block is folded the moment the turn ends, so this
        // is the only way to read what it did.
        if (not grid_active() and viewport_box.Contain(mouse.x, mouse.y)) {
          for (TranscriptNode& node : transcript) {
            if (hit_test(node, mouse.x, mouse.y)) return true;
          }
        }
        // Fall through: a click elsewhere still belongs to the input.
      }
      const bool up = mouse.button == f::Mouse::WheelUp;
      const bool down = mouse.button == f::Mouse::WheelDown;
      if (up or down) {
        std::lock_guard<std::mutex> lock(mutex);
        if (grid_active()) return true;
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

  shutting_down.store(true);
  // Drop the observer before these locals go out of scope: a subagent thread
  // still in flight must not call back into freed state. set_observer takes the
  // same mutex emit() holds while calling, so once it returns no callback is
  // running and none can start.
  agent::AgentPool::instance().set_observer({});

  // The turn thread (possibly parked in save()/assemble_tree waiting on a
  // subagent) and any detached memory thread still hold references into this
  // frame, and neither goes through the observer. Returning into their backs is
  // the one way /quit can corrupt memory, so drain before unwinding — and say
  // why the exit is not instant, since the screen is already gone.
  constexpr int kQuitDrainMs = 30000;
  bool announced = false;
  for (int waited_ms = 0;; waited_ms += 50) {
    {
      if (threads_in_flight.load() == 0) break;
      std::lock_guard<std::mutex> lock(mutex);
      if (not announced) {
        std::cerr << "waiting for work in flight to finish before exiting";
        if (not running_agents.empty()) {
          std::cerr << " (" << running_agents.size()
                    << " subagent(s) still running)";
        }
        std::cerr << "; Ctrl+C to abandon it\n";
        announced = true;
      }
    }
    if (waited_ms >= kQuitDrainMs) {
      // Still busy. Leave without unwinding rather than free state a detached
      // thread is writing to; main()'s flush is skipped along with everything
      // else, so do it here.
      if (memory.enabled) {
        std::string error;
        if (vdb::MemoryStore* store = open_store(memory, error)) {
          store->flush();
        }
      }
      std::cerr << "still busy after " << (kQuitDrainMs / 1000)
                << "s; exiting anyway\n";
      std::_Exit(0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
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
      "Defaults can be set in $XDG_CONFIG_HOME/sp/config (~/.config/sp/config;\n"
      "one KEY=VALUE per line; keys: MODEL, MAX_STEPS, NUM_CTX, SUMMARIZE_AT,\n"
      "OLLAMA_JOBS, ENABLE_SUBAGENTS, MAX_DEPTH, MAX_AGENTS, ENABLE_MEMORY,\n"
      "MEMORY_PATH, MEMORY_EMBED_MODEL) and, per directory, in\n"
      "./.m8trix/settings.json, which wins over it. A flag wins over both.\n"
      "\nsp delegates independent subtasks to subagents (MAX_DEPTH=3,\n"
      "MAX_AGENTS=8), each with its own shell and its own context but the same\n"
      "memory. They share one ollama, so OLLAMA_JOBS is what decides whether\n"
      "they actually overlap.\n"
      "\nsp keeps its files under $XDG_DATA_HOME/sp (~/.local/share/sp):\n"
      "  bin/  commands it installs   src/  sources it keeps   trash/\n"
      "  memory.m8db\n"
      "Sessions go to $XDG_STATE_HOME/sp/sessions, caches to $XDG_CACHE_HOME/sp.");

  const std::string username = get_username();
  const std::string home     = get_home();

  // Resolved once, here, rather than left to the model: where a script
  // belongs, where sources are kept, and whether the bin directory is on the
  // user's PATH are facts about this machine. XDG rather than a pile of
  // $HOME dotfiles, so sp's files sit where a shell tool's files are expected
  // to be and can be found, backed up or deleted as a unit.
  const auto env_or_empty = [](const char* name) -> std::string {
    const char* value = std::getenv(name);
    return value != nullptr ? value : std::string();
  };
  sp::XdgEnv xdg;
  xdg.data_home   = env_or_empty("XDG_DATA_HOME");
  xdg.config_home = env_or_empty("XDG_CONFIG_HOME");
  xdg.state_home  = env_or_empty("XDG_STATE_HOME");
  xdg.cache_home  = env_or_empty("XDG_CACHE_HOME");

  const sp::SpPaths paths =
      sp::resolve_sp_paths(home, xdg, env_or_empty("PATH"));

  std::string paths_error;
  if (not sp::ensure_sp_dirs(paths, paths_error)) {
    // Not fatal: sp still runs, it just cannot install anything. Saying so
    // beats refusing to start over a directory most tasks never touch.
    std::cerr << "warning: " << paths_error
              << "; scripts cannot be installed this run\n";
  }

  // Anything sp left in its old locations follows it across, once, and the
  // user is told rather than left to discover their files moved.
  std::string migration_notes;
  sp::migrate_legacy_paths(home, paths, migration_notes);
  if (not migration_notes.empty()) std::cerr << migration_notes;

  if (not paths.bin_on_path) {
    std::cerr << "note: " << paths.bin()
              << " is not on your PATH — add `export PATH=\"" << paths.bin()
              << ":$PATH\"` to your shell rc file to run commands sp installs "
                 "by name\n";
  }

  // sp's own cache, not the shared ~/.m8trix one.
  tools::set_bash_search_index_path(paths.search_index());

  std::string settings_warning;
  agent::StartupSettings settings =
      agent::load_shellrc_settings(paths.config_file(), settings_warning);
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
  // Subagents are on by default: a shell task often splits into parts that
  // don't need each other's output, and each subagent gets its own bash_repl,
  // so they cannot tread on one another's shell state. 8 rather than core's 16
  // because every live agent is a bash process and a queued Ollama request.
  int max_depth = settings.max_depth.value_or(3);
  int max_agents = settings.max_agents.value_or(8);
  bool subagents = settings.enable_subagents.value_or(true);

  std::string memory_path =
      settings.memory_path.value_or(paths.memory());
  std::string memory_model =
      settings.memory_embed_model.value_or(oc::kDefaultEmbedModel);
  int ollama_jobs = settings.ollama_jobs.value_or(oc::kDefaultOllamaJobs);
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
  app.add_option("--ollama-jobs", ollama_jobs,
                 "Max concurrent requests against ollama, chat and embed "
                 "together")
      ->capture_default_str();
  app.add_flag("--subagents,!--no-subagents", subagents,
               "Let sp delegate independent subtasks to subagents "
               "(default: on)");
  app.add_option("--max-depth", max_depth,
                 "How deep subagents may nest (the root is depth 0)")
      ->capture_default_str();
  app.add_option("--max-agents", max_agents,
                 "How many subagents may be live at once")
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

  oc::OllamaClient::set_concurrency(ollama_jobs);
  oc::OllamaClient::configure(model);
  oc::OllamaClient::configure_embed(memory_model);

  int64_t window = num_ctx;
  if (window <= 0) {
    window = oc::OllamaClient::instance().context_length(model);
    if (window <= 0 and not interactive) {
      std::cerr << "warning: could not detect context length for '" << model
                << "'; auto-summarizing at a flat " << 200000 << " tokens\n";
    }
  }
  if (window > 0) oc::OllamaClient::set_num_ctx(window);

  // Resolve memory last, because the probe is a call to the same Ollama the
  // model validation above has already shown to be up.
  MemoryConfig memory;
  memory.options.path = memory_path;
  memory.options.embed_model = memory_model;
  const std::string memory_mismatch =
      vdb::memory_model_mismatch(memory_path, memory_model);
  if (not memory_mismatch.empty()) {
    // Two models of the same width would otherwise open, write and rank
    // against each other with nothing to show for it but worse recall.
    std::cerr << "warning: " << memory_mismatch
              << " Memory is off for this run.\n";
  } else if (memory_wanted.value_or(true)) {
    const bool usable = vdb::memory_available(memory_model);
    if (usable) {
      memory.enabled = true;
    } else if (memory_wanted.has_value()) {
      // Asked for outright: honour it and say why the calls will fail, rather
      // than silently overruling the user. Same wording as m8trixparrot.
      memory.enabled = true;
      std::cerr << "warning: memory is enabled but '" << memory_model
                << "' is not a pulled embedding model (try `ollama pull "
                << memory_model << "`); memory calls will fail\n";
    } else {
      // Nobody asked either way, so off is the safe read — one line to stderr
      // so a script's stdout stays clean.
      std::cerr << "note: long-term memory is off — `ollama pull "
                << memory_model << "` turns it on\n";
    }
  }

  // Clamped before anything reads them. max_depth = 0 would be the one
  // genuinely broken state: can_spawn_subagents goes false, so the prompt says
  // nothing about subagents, while tool_schemas() still advertises
  // subagent_create — the model would call a tool it was never told about and
  // be refused every time. --no-subagents collapses both caps so the prompt's
  // slot arithmetic can never promise what AgentPool::spawn would refuse.
  max_depth = std::max(1, max_depth);
  max_agents = std::max(1, max_agents);
  if (not subagents) {
    max_depth = 1;
    max_agents = 1;
  }

  // Live agents and *thinking* agents are two different numbers: every agent's
  // model call queues in the one OllamaClient work pool. Saying so beats
  // leaving a deliberately serial run looking like a stall.
  if (subagents and ollama_jobs < 2) {
    std::cerr << "note: subagents are on but --ollama-jobs is " << ollama_jobs
              << ", so they will take turns thinking rather than overlap\n";
  }

  agent::AgentOptions options;
  options.enable_python          = false;
  options.enable_subagents       = subagents;
  options.enable_skills          = false;
  options.enable_package_install = false;
  options.enable_file_tools      = false;
  options.enable_web_search      = false;
  options.enable_bash_search     = true;
  // One shell for the whole run: sp's tasks are shell work, and a shell that
  // forgets everything between calls makes the model redo its setup each time.
  options.enable_bash_repl       = true;
  // Otherwise every turn drops a .m8trix/sessions directory into whatever
  // directory the user happened to be standing in.
  options.session_dir            = paths.sessions();
  options.max_steps              = max_steps;
  // These two feed the prompt (PromptFacts::max_depth / max_agents /
  // free_agent_slots / can_spawn_subagents); AgentPool::configure below is what
  // subagent_create is actually refused by. Both are set from the same pair of
  // locals, here and nowhere else, so they cannot drift apart.
  options.max_depth              = max_depth;
  options.max_agents             = max_agents;
  options.context_window_tokens  =
      static_cast<int>(std::max<int64_t>(0, window));
  options.context_summarize_at_tokens =
      settings.summarize_at.value_or(200000);
  options.enable_memory          = memory.enabled;
  options.memory_path            = memory.options.path;
  options.memory_embed_model     = memory.options.embed_model;
  // sp's prompt is entirely its own — no coding-agent preamble, no agent-tree
  // sentence, no git workspace block. What the model is told about memory
  // follows facts.enable_memory, which mirrors options.enable_memory above.
  options.system_prompt_builder   =
      [username, home, paths](const agent::PromptFacts& facts) {
        return sp::make_system_prompt(facts, username, home, paths);
      };

  agent::AgentPool::configure(options.max_agents, options.max_depth);

  const policy::YoloPolicy pol;
  const std::string root_id =
      agent::AgentPool::instance().register_root("sp");
  agent::Agent root_agent(options, pol, root_id, "", 0);

  const int status = interactive ? run_interactive(root_agent, model, memory)
                                 : run_single_shot(prompt, root_agent);

  // Records reach the file as they are written, so nothing is lost without
  // this; flushing rewrites the header and search snapshot so the next process
  // opens without replaying the whole log. A store nothing ever touched
  // flushes to a no-op.
  if (memory.enabled) {
    std::string error;
    if (vdb::MemoryStore* store = open_store(memory, error)) store->flush();
  }
  return status;
}
