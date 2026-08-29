#ifndef TRANSCRIPT_VIEW_H
#define TRANSCRIPT_VIEW_H

#include <cstddef>
#include <cstdint>

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include <core/agent_result.h>

// The transcript model and its FTXUI rendering, shared by the agent TUIs
// (m8trixparrot, m8trixsh). An app owns a `std::list<TranscriptNode>`, mutates
// it from the AgentPool observer, and renders it with render_node(); the fold
// state and click hit-testing live on the nodes.
namespace agentui {

// A box no click can ever fall inside. Every header box is reset to this at the
// top of a render pass, so a header that isn't drawn this frame can't be hit by
// a click landing where it used to be.
inline constexpr ftxui::Box kNoBox = {1, 0, 1, 0};

// A tool's output can be thousands of lines; the transcript shows the head.
inline constexpr std::size_t kMaxResultLines = 6;

// One tool call and whatever it produced. Collapsing hides the result and
// leaves the header, which is the part that says what was run.
struct ToolSegment {
  std::string tool_name;
  std::string summary;  // The call's key arguments, on one line.
  std::string result;   // Tool output, or the policy's refusal.
  bool denied = false;
  bool expanded = true;  // Cleared when the turn finishes.
  ftxui::Box header_box = kNoBox;
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
  ftxui::Box header_box = kNoBox;
};

// The head of `text`, plus "[... N more lines]" when it was longer.
std::string clip_lines(const std::string& text, std::size_t max_lines);

// Appends a plain node and returns it.
TranscriptNode& add_node(std::list<TranscriptNode>& nodes,
                         TranscriptNode::Kind kind, std::string text);

// Appends a tool segment, starting a new ToolGroup when the previous node isn't
// one — an assistant message between calls means a new round of tool use.
ToolSegment& open_segment(std::list<TranscriptNode>& transcript);

// Resets every header_box in the subtree to kNoBox.
void reset_boxes(TranscriptNode& node);

// Renders `node` (and its children) into `lines`, indented by `indent` levels.
void render_node(std::vector<ftxui::Element>& lines, TranscriptNode& node,
                 int indent);

// One bordered tile for a running subagent, its activity auto-scrolled.
ftxui::Element render_pane(TranscriptNode& sub);

// Fills a trailing cell of the last subagent-grid row.
ftxui::Element empty_pane();

// Toggles the header the click at (x, y) landed on, anywhere in the tree.
// Returns true when it consumed the click.
bool hit_test(TranscriptNode& node, int x, int y);

// True when any ToolGroup/Subagent in the subtree is expanded.
bool any_group_expanded(const TranscriptNode& node);

// Sets the expanded flag on every ToolGroup/Subagent in the subtree.
void set_all_expanded(TranscriptNode& node, bool expand);

// Folds `node` and everything under it.
void collapse_subtree(TranscriptNode& node);

// Renders a loaded result tree read-only into `out`: the conclusion, then a
// folded Subagent block per child, recursively.
void render_result_body(std::list<TranscriptNode>& out,
                        const agent::AgentResult& result);

// "128k" / "1.5M" / "640".
std::string human_tokens(std::int64_t n);

// The subagent grid's shape for `n` running panes: cols = ceil(sqrt(n)),
// rows = ceil(n / cols).
struct GridShape {
  int cols;
  int rows;
};
GridShape grid_shape(int n);

// Drops routing-map entries for `node` and every Subagent under it before the
// list it lives in is cleared (a stale pointer into a cleared std::list would
// dangle).
void forget_subtree(
    TranscriptNode& node,
    std::unordered_map<std::string, TranscriptNode*>& subagent_nodes,
    std::unordered_map<std::string, std::list<TranscriptNode>*>&
        agent_containers);

}  // namespace agentui

#endif  // TRANSCRIPT_VIEW_H
