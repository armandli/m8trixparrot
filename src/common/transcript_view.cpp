#include <common/transcript_view.h>

#include <cstdio>

#include <sstream>
#include <string>
#include <utility>

namespace f = ftxui;

namespace agentui {

std::string clip_lines(const std::string& text, std::size_t max_lines) {
  std::istringstream stream(text);
  std::string line;
  std::string clipped;
  std::size_t count = 0;

  while (std::getline(stream, line)) {
    if (count >= max_lines) {
      std::size_t remaining = 1;
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

TranscriptNode& add_node(std::list<TranscriptNode>& nodes,
                         TranscriptNode::Kind kind, std::string text) {
  TranscriptNode node;
  node.kind = kind;
  node.text = std::move(text);
  nodes.push_back(std::move(node));
  return nodes.back();
}

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

void reset_boxes(TranscriptNode& node) {
  node.header_box = kNoBox;
  for (ToolSegment& segment : node.segments) segment.header_box = kNoBox;
  for (TranscriptNode& child : node.children) reset_boxes(child);
}

namespace {

f::Element indent_line(int indent, f::Element element) {
  if (indent <= 0) return element;
  return f::hbox({f::text(std::string(2 * indent, ' ')), std::move(element)});
}

}  // namespace

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
      const std::size_t count = node.segments.size();
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

namespace {

void render_pane_body(std::vector<f::Element>& lines, TranscriptNode& sub) {
  if (sub.children.empty()) {
    lines.push_back(f::text("(starting...)") | f::dim);
    return;
  }
  for (TranscriptNode& child : sub.children) render_node(lines, child, 0);
}

}  // namespace

f::Element render_pane(TranscriptNode& sub) {
  std::vector<f::Element> body;
  render_pane_body(body, sub);
  f::Element title = f::hbox({
      f::text("d" + std::to_string(sub.depth) + "  ") | f::bold |
          f::color(f::Color::Blue),
      f::text(clip_lines(sub.objective, 1)) | f::bold,
      f::text("  (running)") | f::dim,
  });
  f::Element content = f::vbox(std::move(body)) |
                       f::focusPositionRelative(0.f, 1.f) | f::yframe | f::flex;
  return f::window(std::move(title), std::move(content)) | f::flex;
}

f::Element empty_pane() { return f::filler() | f::border | f::flex; }

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

void collapse_subtree(TranscriptNode& node) {
  if (node.kind == TranscriptNode::Kind::ToolGroup) {
    node.expanded = false;
    for (ToolSegment& segment : node.segments) segment.expanded = false;
  } else if (node.kind == TranscriptNode::Kind::Subagent) {
    node.expanded = false;
  }
  for (TranscriptNode& child : node.children) collapse_subtree(child);
}

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

std::string human_tokens(std::int64_t n) {
  if (n < 1000) return std::to_string(n);
  if (n < 1000000) return std::to_string((n + 500) / 1000) + "k";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1000000.0);
  return std::string(buf);
}

GridShape grid_shape(int n) {
  int cols = 1;
  while (cols * cols < n) ++cols;
  return {cols, (n + cols - 1) / cols};
}

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

}  // namespace agentui
