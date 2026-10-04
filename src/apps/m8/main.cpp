#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

#include <common/interaction_queue.h>
#include <common/transcript_view.h>
#include <core/agent/agent.h>
#include <core/agent/agent_pool.h>
#include <core/vdb/memory_ops.h>
#include <core/vdb/memory_store.h>
#include <core/agent/agent_settings.h>
#include <core/mcp/config.h>
#include <core/mcp/content.h>
#include <core/mcp/elicitation.h>
#include <core/mcp/registry.h>
#include <core/mcp/tool_search.h>
#include <core/policy/policy.h>
#include <core/policy/sane_policy.h>
#include <core/tools/tools.h>
#include <core/util/open_url.h>

#include <m8_mcp_cli.h>
#include <m8_mcp_view.h>
#include <m8_paths.h>
#include <m8_prompt.h>

namespace f = ftxui;

namespace {

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

// The argument of a /command, with surrounding whitespace dropped so
// "/remember  x  " and "/remember x" store the same thing.
std::string trim_copy(std::string text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end and std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin and
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

const char* kHelpText =
    "/help     show this message\n"
    "/session  show the current session id\n"
    "/context  show context token usage and the auto-summarize threshold\n"
    "/skills   list available skills (and re-scan the skills directory)\n"
    "/mcp      MCP servers and tool search; /mcp help for its subcommands\n"
    "/remember <text>   store something about this project in memory\n"
    "/memories [query]  search memory, or show statistics with no query\n"
    "/forget <id>       delete one memory by id\n"
    "/reset    start a new session\n"
    "/quit     exit\n"
    "\n"
    "click a > / v header to fold or unfold that tool call, group, or subagent\n"
    "Ctrl+T    fold or unfold everything at once\n"
    "Ctrl+G    while subagents run: toggle the pane grid / the transcript";

const char* kMcpHelpText =
    "/mcp                          servers, their state, and tool search\n"
    "/mcp tools [server]           every MCP tool: loaded, deferred or always loaded\n"
    "/mcp resources [server]       what the servers offer as resources\n"
    "/mcp reconnect [server]       restart a connection (all of them without a name)\n"
    "/mcp enable|disable <server>  turn a server on or off in this workspace\n"
    "/mcp approve <server>|--all   let a server from a workspace file run\n"
    "/mcp login|logout <server>    log in to a remote server (OAuth), or forget it\n"
    "/mcp__<server>__<prompt> [args]  run a server's prompt (listed in /help)\n"
    "\n"
    "servers are added from a shell: m8 mcp add <name> -- <command> [args...]";

// The elicitation engine (core) and the interaction queue (the UI's) each
// have their own question type; m8 is where they meet.
agentui::Question to_ui(const mcp::Question& question) {
  agentui::Question out;
  out.heading = question.heading;
  out.message = question.message;
  out.detail = question.detail;
  out.emphasis = question.emphasis;
  out.problem = question.problem;
  out.progress = question.progress;
  out.initial = question.initial;
  out.keys = question.keys;
  return out;
}

mcp::Response to_mcp(const agentui::Answer& answer) {
  mcp::Response out;
  out.text = answer.text;
  switch (answer.kind) {
    case agentui::Answer::Kind::Answered:
      out.kind = mcp::Response::Kind::Answered;
      break;
    case agentui::Answer::Kind::Declined:
      out.kind = mcp::Response::Kind::Declined;
      break;
    case agentui::Answer::Kind::Cancelled:
      out.kind = mcp::Response::Kind::Cancelled;
      break;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  // The transcript model and its renderers now live in the shared agentui
  // library; pull the names in for this whole function.
  using namespace agentui;

  // Whether this m8 runs inside another m8's shell (its agent's bash_repl, or
  // a `!command`). Read before this process marks its own children below.
  const bool agent_shell = std::getenv("M8_AGENT_SHELL") != nullptr;

  CLI::App app{"m8 - a coding agent over Ollama"};
  app.footer(
      "Defaults for model, policy, and the flags below may also be set in "
      "<workspace>/.m8/config.json (keys: model, policy, max_steps, max_depth, "
      "max_agents, num_ctx, summarize_at, ollama_jobs, skills_dir, "
      "enable_skills, enable_subagents, enable_bash_repl, enable_bash_search, "
      "enable_memory, memory_path, memory_embed_model, enable_web_search, "
      "enable_mcp, tool_search, mcp_oauth_client_metadata_url); an explicit "
      "flag here always overrides it. MCP servers: m8 mcp --help.");

  // Everything m8 keeps lives in <workspace>/.m8, where <workspace> is the
  // nearest ancestor of the cwd carrying a .git or .m8 marker. Resolved before
  // anything else so the config path, the skills directory, the session
  // directory and the memory file are all one decision rather than four.
  const char* home_env = std::getenv("HOME");
  const m8::M8Paths paths = m8::resolve_m8_paths(
      std::filesystem::current_path().string(), home_env ? home_env : "");

  std::string dirs_error;
  if (not m8::ensure_m8_dirs(paths, dirs_error)) {
    // Not fatal: m8 runs fine without being able to persist, and saying so once
    // beats failing on every session write.
    std::cerr << "warning: " << dirs_error
              << "; sessions, skills and memory are unavailable\n";
  } else if (not m8::ensure_default_config(paths, dirs_error)) {
    std::cerr << "warning: " << dirs_error << "\n";
  }

  // The bash_search index describes PATH, which belongs to the machine rather
  // than to this repository, so it stays shared in ~/.m8. With no HOME the
  // built-in default (which falls back to /tmp) is left alone.
  if (not paths.search_index().empty()) {
    tools::set_bash_search_index_path(paths.search_index());
  }

  // .m8/config.json (if present) supplies defaults for the flags below — loaded
  // before the flags are declared so CLI11's ->capture_default_str() reflects
  // it, and an explicit flag on the command line still overwrites whatever the
  // file set, since CLI11 assigns into the same variable.
  std::string settings_warning;
  const agent::StartupSettings settings =
      agent::load_startup_settings(paths.config_file(), settings_warning);
  if (not settings_warning.empty()) {
    std::cerr << "warning: " << settings_warning << "\n";
  }

  std::string model = settings.model.value_or("qwen3.8:27b-mlx");
  std::string resume_id;
  bool resume_latest = false;
  int max_steps = settings.max_steps.value_or(12);
  int max_depth = settings.max_depth.value_or(3);
  int max_agents = settings.max_agents.value_or(16);
  int num_ctx = settings.num_ctx.value_or(0);
  int summarize_at = settings.summarize_at.value_or(200000);
  int ollama_jobs = settings.ollama_jobs.value_or(oc::kDefaultOllamaJobs);
  std::string skills_dir = settings.skills_dir.value_or(paths.skills());
  bool no_skills = not settings.enable_skills.value_or(true);

  app.add_option("model,-m,--model", model, "Ollama model to run the agent on")
      ->capture_default_str();
  app.add_flag("-r,--resume", resume_latest,
               "Resume the most recent session in .m8/sessions");
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
  app.add_option("--ollama-jobs", ollama_jobs,
                 "Max concurrent requests against ollama, chat and embed "
                 "together")
      ->capture_default_str();
  app.add_option("--summarize-at", summarize_at,
                 "Auto-summarize the transcript at this many tokens "
                 "(also capped at 80% of the context window)")
      ->capture_default_str();
  app.add_option("--skills-dir", skills_dir,
                 "Directory to load skills from (<dir>/<name>/SKILL.md)")
      ->capture_default_str();
  app.add_flag("--no-skills", no_skills, "Disable the skill system");

  bool no_mcp = not settings.enable_mcp.value_or(true);
  std::string tool_search_mode = settings.tool_search.value_or("auto");
  app.add_flag("--no-mcp", no_mcp, "Start no MCP servers this run");
  app.add_option("--tool-search", tool_search_mode,
                 "When MCP tool schemas wait behind tool_search: auto, auto:N "
                 "(past N% of the context window), on or off")
      ->capture_default_str()
      ->check([](const std::string& value) {
        return mcp::ToolSearchSettings::parse(value)
                   ? std::string()
                   : std::string("expected auto, auto:N, on or off");
      });

  m8::McpCli mcp_cli;
  mcp_cli.declare(app);

  std::string policy_name = settings.policy.value_or("sane");
  app.add_option("-p,--policy", policy_name,
                 "Permission policy: yolo (allow everything) or sane "
                 "(no su/sudo, writes confined to the working directory "
                 "and /tmp)")
      ->capture_default_str()
      ->check(CLI::IsMember({"yolo", "sane"}));

  CLI11_PARSE(app, argc, argv);

  // `m8 mcp ...` manages servers and exits; it needs no model.
  if (mcp_cli.parsed()) {
    mcp_cli.interactive = ::isatty(STDIN_FILENO) == 1 and ::isatty(STDOUT_FILENO) == 1;
    mcp_cli.client_metadata_url = settings.mcp_oauth_client_metadata_url.value_or("");
    return mcp_cli.run(paths, agent_shell, std::cout, std::cerr, std::cin);
  }

  // Every shell this m8 starts — its agent's bash_repl, a `!command` — inherits
  // the mark, so `m8 mcp add` or `approve` run from one refuses: which servers
  // m8 runs is the user's call, not the agent's. Set before any thread exists,
  // since setenv is not safe against a concurrent getenv.
  ::setenv("M8_AGENT_SHELL", "1", 1);

  // A config-file value never went through the flag's check.
  std::optional<mcp::ToolSearchSettings> tool_search =
      mcp::ToolSearchSettings::parse(tool_search_mode);
  if (not tool_search) {
    std::cerr << "warning: tool_search \"" << tool_search_mode
              << "\" is not auto, auto:N, on or off; using auto\n";
    tool_search = mcp::ToolSearchSettings{};
  }

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
  //
  // The write root is the workspace root, not the launch directory: everything
  // else m8 does with paths walks up to the nearest .git/.m8, and a policy whose
  // boundary sat at whatever subdirectory you happened to start in would refuse
  // writes to sibling directories of the same project.
  const policy::YoloPolicy yolo_policy;
  const policy::SanePolicy sane_policy(paths.root);
  const policy::PolicyInterface& pol =
      policy_name == "sane"
          ? static_cast<const policy::PolicyInterface&>(sane_policy)
          : static_cast<const policy::PolicyInterface&>(yolo_policy);

  oc::OllamaClient::set_concurrency(ollama_jobs);
  oc::OllamaClient::configure(model);
  agent::AgentPool::configure(max_agents, max_depth);

  // m8 reads and writes files by running the installed tool_* commands in its
  // shell, so which of them exist is a startup question, not something to
  // discover one failed tool call at a time. What is found goes into the system
  // prompt; what is missing is named once here. Not fatal — the model still has
  // a whole shell, it just has to make do with cat and sed.
  std::vector<std::string> wanted_tools;
  for (const m8::InstalledTool& tool : m8::known_installed_tools()) {
    wanted_tools.push_back(tool.name);
  }
  const char* path_env = std::getenv("PATH");
  const std::vector<std::string> installed_tools =
      m8::find_on_path(wanted_tools, path_env ? path_env : "");
  if (installed_tools.size() < wanted_tools.size()) {
    std::string missing;
    for (const std::string& name : wanted_tools) {
      if (std::find(installed_tools.begin(), installed_tools.end(), name) ==
          installed_tools.end()) {
        missing += (missing.empty() ? "" : ", ") + name;
      }
    }
    std::cerr << "warning: not on PATH: " << missing
              << "\n         run `make install` (or add the build directory to "
                 "PATH) so the agent can use them\n";
  }

  int64_t window = num_ctx;
  if (window <= 0) {
    window = oc::OllamaClient::instance().context_length(model);
    if (window <= 0) {
      std::cerr << "warning: could not detect the context length for '" << model
                << "'; auto-summarizing at a flat " << summarize_at
                << " tokens\n";
    }
  }
  if (window > 0) oc::OllamaClient::set_num_ctx(window);

  agent::AgentOptions options;
  options.max_steps = max_steps;
  options.max_depth = max_depth;
  options.max_agents = max_agents;
  options.context_window_tokens = static_cast<int>(std::max<int64_t>(0, window));
  options.context_summarize_at_tokens = summarize_at;
  options.skills_dir = skills_dir;
  options.enable_skills = not no_skills;
  // No CLI flag for these three — settings.json is their only knob.
  options.enable_subagents =
      settings.enable_subagents.value_or(options.enable_subagents);
  options.enable_web_search =
      settings.enable_web_search.value_or(options.enable_web_search);
  if (options.enable_web_search and not tools::web_search_available()) {
    std::cerr << "warning: enable_web_search is set but no Parallel API key was "
                 "found (PARALLEL_API_KEY or .m8/parallel_api_key); "
                 "websearch calls will fail\n";
  }

  // m8's tool set. bash_repl is the execution substrate and bash_search is how
  // the model finds what this machine can do; the file and web tools stay off
  // because m8 reaches those through the installed tool_* binaries instead, so
  // the shell holds one coherent view of the work rather than two.
  options.session_dir = paths.sessions();
  options.enable_bash_repl =
      settings.enable_bash_repl.value_or(true);
  options.enable_bash_search =
      settings.enable_bash_search.value_or(true);
  options.enable_file_tools = false;

  options.enable_memory = settings.enable_memory.value_or(true);
  options.memory_path = settings.memory_path.value_or(paths.memory());
  options.memory_embed_model =
      settings.memory_embed_model.value_or(options.memory_embed_model);
  oc::OllamaClient::configure_embed(options.memory_embed_model);
  if (options.enable_memory) {
    // Two models of the same width would otherwise open, write and rank
    // against each other with nothing to show for it but worse recall.
    const std::string mismatch = vdb::memory_model_mismatch(
        options.memory_path, options.memory_embed_model);
    if (not mismatch.empty()) {
      options.enable_memory = false;
      std::cerr << "warning: " << mismatch << " Memory is off for this run.\n";
    } else if (not vdb::memory_available(options.memory_embed_model)) {
      std::cerr << "warning: enable_memory is set but '"
                << options.memory_embed_model
                << "' is not a pulled embedding model (try `ollama pull "
                << options.memory_embed_model << "`); memory calls will fail\n";
    }
  }

  // m8's prompt is its own, like every other application's; core supplies only
  // the facts it is built from. The capture is by value so the builder stays
  // valid inside every subagent, which gets a copy of AgentOptions.
  options.system_prompt_builder = [paths, installed_tools](
                                      const agent::PromptFacts& facts) {
    return m8::make_system_prompt(facts, paths, installed_tools);
  };

  // Questions for the person at the keyboard — an MCP server's form, the
  // agent's ask_user — one session at a time; the answer box takes the
  // keyboard while one waits.
  agentui::InteractionQueue interactions;
  // How a background thread adds to the transcript; set once the transcript
  // exists, but captured now, before the root agent copies the options.
  std::function<void(TranscriptNode::Kind, std::string)> note;

  options.ask_user_handler = [&interactions, &note](const std::string& prompt) -> std::string {
    agentui::InteractionQueue::Session session = interactions.begin();
    if (note) note(TranscriptNode::Kind::Notice, "\xe2\x97\x86 the agent asks: " + prompt);
    agentui::Question question;
    question.heading = "the agent asks";
    question.message = prompt;
    question.keys = "Enter answers \xc2\xb7 Esc leaves it unanswered";
    const agentui::Answer answer = session.ask(std::move(question));
    if (answer.kind != agentui::Answer::Kind::Answered) {
      if (note) note(TranscriptNode::Kind::Notice, "(left unanswered)");
      return std::string();
    }
    if (note) note(TranscriptNode::Kind::User, answer.text);
    return answer.text;
  };

  // MCP servers. The registry exists whenever MCP is on, even with nothing
  // configured, so /mcp can say how to add a server. It is deliberately never
  // freed (see mcp::Registry): agent threads are detached and may still hold it
  // as the process exits. Its servers start below, once notices have somewhere
  // to go.
  mcp::Registry* registry = nullptr;
  mcp::LoadedConfig mcp_config;
  if (not no_mcp) {
    mcp::ConfigFiles files;
    files.user = paths.user_mcp_config();
    files.shared = paths.shared_mcp_config();
    files.project = paths.mcp_config();
    mcp_config = mcp::load_config(files);

    mcp::RegistryOptions registry_options;
    registry_options.workspace = m8::workspace_key(paths);
    registry_options.logs_dir = paths.mcp_logs();
    registry_options.state_path = paths.mcp_state();
    registry_options.trust_path = paths.mcp_trust();
    registry_options.credentials_path = paths.mcp_credentials();
    registry_options.client_metadata_url = settings.mcp_oauth_client_metadata_url.value_or("");
    registry = new mcp::Registry(std::move(registry_options));
    options.mcp = std::shared_ptr<mcp::Toolbox>(registry, [](mcp::Toolbox*) {});
  }
  options.tool_search = *tool_search;

  const std::string root_id = agent::AgentPool::instance().register_root("root");
  agent::Agent root_agent(options, pol, root_id, "", 0);

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

  // Subagent ids currently running, in spawn order. An unordered_map iteration
  // order is unstable and would make panes jump between frames. Ids, not
  // TranscriptNode*: on ContextSummarized the owning std::list is cleared, so a
  // cached pointer could dangle; resolving through `subagent_nodes` at render
  // time lets a pruned pane simply vanish. Guarded by `mutex`.
  std::vector<std::string> running_agents;
  // Ctrl+G while subagents run: show the transcript instead of the pane grid.
  // Reset to false whenever `running_agents` empties. Guarded by `mutex`.
  bool force_conversation_view = false;

  auto screen = f::App::Fullscreen();

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
          segment.tool_name = m8::tool_display_name(event.tool_name);
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
          running_agents.push_back(event.agent_id);
          break;
        }

        case agent::AgentEvent::Kind::SubagentDone: {
          if (auto it = subagent_nodes.find(event.agent_id);
              it != subagent_nodes.end()) {
            it->second->done = true;
            it->second->ok = event.ok;
            collapse_subtree(*it->second);
          }
          std::erase(running_agents, event.agent_id);
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
          // A subagent whose node was just pruned (or already finished) is no
          // longer a live pane.
          std::erase_if(running_agents, [&](const std::string& id) {
            auto it = subagent_nodes.find(id);
            return it == subagent_nodes.end() or it->second->done;
          });
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
  // push_notice from a background thread, which also has to wake the UI loop:
  // nothing redraws until the next event otherwise.
  auto post_notice = [&](TranscriptNode::Kind kind, std::string text) {
    push_notice(kind, std::move(text));
    screen.PostEvent(f::Event::Custom);
  };
  // Whether a turn is running. Read under the lock: the turn thread writes it.
  auto busy = [&] {
    std::lock_guard<std::mutex> lock(mutex);
    return waiting_for_reply;
  };

  note = post_notice;
  interactions.set_listener([&screen] { screen.PostEvent(f::Event::Custom); });

  // Links the person agreed to open, and servers they would not log in to;
  // only the question session being answered touches either.
  std::set<std::string> opened_links;
  std::set<std::string> login_declined;
  if (registry != nullptr) {
    // A server asking the person something mid-call. Runs on whichever thread
    // waits for that server, and holds the question queue until it is done.
    registry->set_elicitation_handler([&](const mcp::ElicitationRequest& request) {
      agentui::InteractionQueue::Session session = interactions.begin();
      if (shutting_down.load()) return mcp::ElicitationResult{};
      const std::string who = request.server + (request.agent_label.empty()
                                                    ? std::string()
                                                    : " (for " + request.agent_label + ")");
      post_notice(TranscriptNode::Kind::Notice,
                  "mcp: " + who + (request.mode == "url" ? " asks you to open a link: " : " asks: ") +
                      request.message);
      const mcp::ElicitationResult result = mcp::run_elicitation(
          request,
          [&session](const mcp::Question& question) {
            return to_mcp(session.ask(to_ui(question)));
          },
          util::open_url, &opened_links);
      if (not shutting_down.load()) {
        post_notice(TranscriptNode::Kind::Notice,
                    result.action == "accept"    ? "mcp: answered " + request.server
                    : result.action == "decline" ? "mcp: declined " + request.server + "'s request"
                                                 : "mcp: cancelled " + request.server + "'s request");
      }
      return result;
    });
    // An agent's call found its login expired, or too narrow: ask, and log in
    // while the answer box waits with the link (Esc gives up). A server the
    // person says no to is not asked about again this run.
    registry->set_login_prompt([&](const std::string& server, const std::string& reason) {
      agentui::InteractionQueue::Session session = interactions.begin();
      if (shutting_down.load() or login_declined.count(server) > 0) return false;
      agentui::Question question;
      question.heading = server + " asks you to log in";
      question.message = "The agent's call to " + server + " stopped: " + reason + ".";
      question.keys = "y logs in, in your browser \xc2\xb7 Enter or n skips \xc2\xb7 Esc cancels";
      const agentui::Answer answer = session.ask(std::move(question));
      std::string word = trim_copy(answer.text);
      for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (answer.kind != agentui::Answer::Kind::Answered or (word != "y" and word != "yes")) {
        login_declined.insert(server);
        post_notice(TranscriptNode::Kind::Notice,
                    "mcp: not logging in to " + server + " (/mcp login " + server + " any time)");
        return false;
      }
      std::atomic<bool> cancel{false};
      std::atomic<bool> finished{false};
      std::mutex link_mutex;
      std::condition_variable link_ready;
      std::string link;
      bool ok = false;
      std::string error;
      std::thread worker([&] {
        ok = registry->login(
            server,
            [&](const std::string& url) {
              {
                std::lock_guard<std::mutex> lock(link_mutex);
                link = url;
              }
              link_ready.notify_all();
              std::string why;
              util::open_url(url, why);
            },
            &cancel, error);
        finished.store(true);
        link_ready.notify_all();
      });
      std::string shown_link;
      {
        std::unique_lock<std::mutex> lock(link_mutex);
        link_ready.wait(lock, [&] { return not link.empty() or finished.load(); });
        shown_link = link;
      }
      if (not shown_link.empty()) {
        agentui::Question waiting;
        waiting.heading = server + ": log in in your browser";
        waiting.message = "Finish logging in there and the agent's call goes on.";
        waiting.detail = "link: " + shown_link;
        waiting.keys = "Esc gives up";
        if (session.ask_until(std::move(waiting), [&] { return finished.load(); })) {
          cancel.store(true);
        }
      }
      worker.join();
      post_notice(ok ? TranscriptNode::Kind::Notice : TranscriptNode::Kind::Error,
                  ok ? "mcp: logged in to " + server : "mcp: login to " + server + " failed: " + error);
      return ok;
    });
    // Called on the registry's threads, never under its lock (it takes ours).
    registry->set_observer([&](const mcp::RegistryEvent& event) {
      if (shutting_down.load()) return;
      if (not event.text.empty()) {
        push_notice(event.state == mcp::ServerState::Failed
                        ? TranscriptNode::Kind::Error
                        : TranscriptNode::Kind::Notice,
                    event.text);
      }
      // A state change redraws the header's mcp tag even without a notice.
      screen.PostEvent(f::Event::Custom);
    });
    for (const std::string& warning : mcp_config.warnings) {
      push_notice(TranscriptNode::Kind::Error, "mcp: " + warning);
    }
    registry->start(std::move(mcp_config.servers));
    // Named once, here; /mcp shows it again.
    const std::string pending = m8::pending_approval_text(*registry->snapshot());
    if (not pending.empty()) push_notice(TranscriptNode::Kind::Notice, pending);
  }

  // Adds `display` as the user turn and runs `objective` (usually the same
  // string; a /command expands it) on a detached thread. `prepare`, when set,
  // first rewrites `objective` on that thread — for work slow enough that the
  // busy flag should cover it, like fetching an MCP prompt — and returns false,
  // with `objective` replaced by why, to end the turn there.
  auto start_turn = [&](std::string display, std::string objective,
                        std::function<bool(std::string&)> prepare = nullptr) {
    std::list<TranscriptNode>::iterator turn_begin;
    {
      std::lock_guard<std::mutex> lock(mutex);
      add_node(transcript, TranscriptNode::Kind::User, std::move(display));
      turn_begin = std::prev(transcript.end());
      waiting_for_reply = true;
      scroll_y = 1.0f;
    }

    std::thread([&root_agent, &mutex, &transcript, &waiting_for_reply,
                &scroll_y, &screen, &running_agents, registry,
                objective = std::move(objective), prepare = std::move(prepare),
                turn_begin]() mutable {
      // Re-lists, in the background, the tools of modern servers whose list
      // has outlived the time-to-live they gave it.
      if (registry != nullptr) registry->refresh_stale();
      agent::AgentResult result;
      if (prepare and not prepare(objective)) {
        result.error = objective;
      } else {
        result = root_agent.run_turn(objective);
      }

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
      // Every subagent has emitted SubagentDone by now; this is belt-and-braces
      // so a missed event can't strand the grid over the transcript.
      running_agents.clear();
      waiting_for_reply = false;
      scroll_y = 1.0f;
      screen.PostEvent(f::Event::Custom);
    }).detach();
  };

  // /mcp [subcommand]. Everything here is the user's own action at the
  // keyboard — which is why approving and enabling servers is allowed here and
  // refused in an agent's shell.
  auto handle_mcp = [&](const std::string& entered) {
    push_notice(TranscriptNode::Kind::User, entered);
    if (registry == nullptr) {
      push_notice(TranscriptNode::Kind::Notice,
                  "MCP is off for this run (--no-mcp, or \"enable_mcp\": false "
                  "in .m8/config.json)");
      return;
    }
    std::istringstream words(entered.substr(4));
    std::string sub;
    std::string name;
    words >> sub >> name;
    const std::shared_ptr<const mcp::Catalog> catalog = registry->snapshot();
    const bool deferred = root_agent.mcp_deferred(*catalog);

    if (sub.empty() or sub == "status") {
      push_notice(TranscriptNode::Kind::Notice,
                  m8::mcp_status_text(*catalog, *tool_search,
                                      root_agent.context_window(), deferred,
                                      root_agent.loaded_mcp_tools()));
    } else if (sub == "tools") {
      push_notice(TranscriptNode::Kind::Notice,
                  m8::mcp_tools_text(*catalog, name, deferred,
                                     root_agent.loaded_mcp_tools()));
    } else if (sub == "resources") {
      // A round trip to every server: off the UI thread.
      std::thread([&post_notice, registry, name] {
        const tools::ToolResult listed =
            registry->resource("list", name, "", mcp::CallContext{});
        const tools::ToolResult templates =
            registry->resource("templates", name, "", mcp::CallContext{});
        if (not listed.ok) {
          post_notice(TranscriptNode::Kind::Error, listed.error);
          return;
        }
        post_notice(TranscriptNode::Kind::Notice,
                    listed.output + (templates.ok ? "\n" + templates.output : ""));
      }).detach();
    } else if (sub == "reconnect") {
      registry->reconnect(name);
      push_notice(TranscriptNode::Kind::Notice,
                  name.empty() ? "mcp: reconnecting every server"
                               : "mcp: reconnecting " + name);
    } else if (sub == "enable" or sub == "disable") {
      std::string error;
      if (name.empty()) {
        push_notice(TranscriptNode::Kind::Notice, "usage: /mcp " + sub + " <server>");
      } else if (registry->set_enabled(name, sub == "enable", error)) {
        push_notice(TranscriptNode::Kind::Notice,
                    "mcp: " + name + (sub == "enable" ? " enabled" : " disabled") +
                        " in this workspace");
      } else {
        push_notice(TranscriptNode::Kind::Error, "mcp: " + error);
      }
    } else if (sub == "approve") {
      const std::vector<std::string> names =
          name == "--all" ? registry->pending_approval()
                          : (name.empty() ? std::vector<std::string>()
                                          : std::vector<std::string>{name});
      if (names.empty()) {
        push_notice(TranscriptNode::Kind::Notice,
                    name == "--all" ? "mcp: no server is waiting for approval"
                                    : "usage: /mcp approve <server>|--all");
        return;
      }
      for (const std::string& server : names) {
        std::string error;
        if (registry->approve(server, error)) {
          push_notice(TranscriptNode::Kind::Notice,
                      "mcp: approved " + server + " for this workspace; connecting");
        } else {
          push_notice(TranscriptNode::Kind::Error, "mcp: " + error);
        }
      }
    } else if ((sub == "login" or sub == "logout") and name.empty()) {
      push_notice(TranscriptNode::Kind::Notice, "usage: /mcp " + sub + " <server>");
    } else if (sub == "login") {
      // Typing it is the consent to open the browser. The wait for the
      // browser (up to five minutes) is off the UI thread.
      std::thread([&post_notice, registry, name] {
        std::string error;
        const bool ok = registry->login(
            name,
            [&](const std::string& url) {
              std::string why;
              const bool opened = util::open_url(url, why);
              post_notice(TranscriptNode::Kind::Notice,
                          "mcp: log in to " + name + (opened ? " in the browser m8 opened" : "") +
                              ":\n" + url +
                              (opened ? std::string() : "\n(could not open a browser: " + why + ")"));
            },
            nullptr, error);
        post_notice(ok ? TranscriptNode::Kind::Notice : TranscriptNode::Kind::Error,
                    ok ? "mcp: logged in to " + name : "mcp: login to " + name + " failed: " + error);
      }).detach();
    } else if (sub == "logout") {
      std::string error;
      if (registry->logout(name, error)) {
        push_notice(TranscriptNode::Kind::Notice, "mcp: logged out of " + name);
      } else {
        push_notice(TranscriptNode::Kind::Error, "mcp: " + error);
      }
    } else {
      push_notice(TranscriptNode::Kind::Notice, kMcpHelpText);
    }
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

    // !<command> runs a bash command directly, without going through the
    // model or the policy layer: it is the user's own explicit command, not
    // something the agent decided to run.
    if (entered.size() > 1 and entered[0] == '!') {
      const std::string command = entered.substr(1);
      push_notice(TranscriptNode::Kind::User, entered);
      std::thread([&push_notice, command] {
        tools::ToolArgs args;
        args["command"] = command;
        const tools::ToolResult result = tools::BashTool().execute(args);
        if (result.ok) {
          push_notice(TranscriptNode::Kind::Assistant, result.output);
        } else {
          push_notice(TranscriptNode::Kind::Error, result.error);
        }
      }).detach();
      return;
    }

    if (entered == "/quit") {
      screen.ExitLoopClosure()();
      return;
    }
    if (entered == "/help") {
      std::string help = kHelpText;
      bool header = false;
      for (const agent::SkillInfo& skill : root_agent.skill_catalog().skills) {
        if (not skill.command) continue;
        if (not header) {
          help += "\n\nskill commands:";
          header = true;
        }
        help += "\n/" + skill.name +
                (skill.argument_hint.empty() ? "" : "  " + skill.argument_hint) +
                " — " + skill.description;
      }
      if (registry != nullptr) {
        const std::string prompts = m8::prompt_help_text(*registry->snapshot());
        if (not prompts.empty()) help += "\n" + prompts;
      }
      push_notice(TranscriptNode::Kind::Notice, help);
      return;
    }
    // Memory by hand. Deliberately the same MemoryStoreRegistry handle the
    // agent's `memory` tool resolves: a second handle on one file would go
    // stale the moment the other wrote, so the user would be shown a view the
    // agent does not have. Resolved per command rather than cached because the
    // store is only created on the first thing anything remembers.
    auto with_memory = [&](const std::function<std::string(vdb::MemoryStore&)>& op) {
      if (not options.enable_memory) {
        push_notice(TranscriptNode::Kind::Error,
                    "memory is off for this run (set enable_memory in "
                    ".m8/config.json, and pull the embedding model)");
        return;
      }
      vdb::MemoryOptions memory_options;
      memory_options.path = options.memory_path;
      memory_options.embed_model = options.memory_embed_model;

      std::string error;
      vdb::MemoryStore* store =
          vdb::MemoryStoreRegistry::instance().get(memory_options, error);
      if (store == nullptr) {
        push_notice(TranscriptNode::Kind::Error, error);
        return;
      }
      // Embedding is an Ollama round trip, so this runs off the UI thread —
      // otherwise a slow model freezes the whole screen. MemoryStore is
      // thread-safe, and the notice lands through the same mutex every other
      // event does.
      std::thread([&push_notice, store, op] {
        push_notice(TranscriptNode::Kind::Notice, op(*store));
      }).detach();
    };

    if (entered == "/remember" or entered.rfind("/remember ", 0) == 0) {
      const std::string text =
          entered.size() > 10 ? trim_copy(entered.substr(10)) : std::string();
      push_notice(TranscriptNode::Kind::User, entered);
      with_memory([text](vdb::MemoryStore& store) {
        return vdb::do_remember(store, text);
      });
      return;
    }
    if (entered == "/memories" or entered.rfind("/memories ", 0) == 0) {
      const std::string query =
          entered.size() > 10 ? trim_copy(entered.substr(10)) : std::string();
      push_notice(TranscriptNode::Kind::User, entered);
      with_memory([query](vdb::MemoryStore& store) {
        return vdb::do_search(store, query);
      });
      return;
    }
    if (entered == "/forget" or entered.rfind("/forget ", 0) == 0) {
      const std::string arg =
          entered.size() > 8 ? trim_copy(entered.substr(8)) : std::string();
      push_notice(TranscriptNode::Kind::User, entered);
      // strtoull would accept "12abc" and a negative id wraps around, so the
      // digits are checked before the conversion rather than after it.
      const bool digits =
          not arg.empty() and
          arg.find_first_not_of("0123456789") == std::string::npos;
      const uint64_t id = digits ? std::strtoull(arg.c_str(), nullptr, 10) : 0;
      if (id == 0) {
        push_notice(TranscriptNode::Kind::Error, "usage: /forget <memory id>");
        return;
      }
      with_memory([id](vdb::MemoryStore& store) {
        return vdb::do_forget(store, id);
      });
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
      if (registry != nullptr) {
        const auto catalog = registry->snapshot();
        const std::string line = m8::mcp_context_line(
            *catalog, root_agent.mcp_deferred(*catalog), root_agent.loaded_mcp_tools());
        if (not line.empty()) msg += "\n" + line;
      }
      push_notice(TranscriptNode::Kind::Notice, msg);
      return;
    }
    if (entered == "/reset") {
      // reset() under a running turn would pull its transcript out from under it.
      if (busy()) {
        push_notice(TranscriptNode::Kind::Notice,
                    "the agent is working; /reset once it is done");
        return;
      }
      root_agent.reset();
      {
        std::lock_guard<std::mutex> lock(mutex);
        transcript.clear();
        subagent_nodes.clear();
        agent_containers.clear();
        agent_containers[root_id] = &transcript;
        running_agents.clear();
        force_conversation_view = false;
      }
      push_notice(TranscriptNode::Kind::Notice, "started a new session");
      return;
    }
    if (entered == "/skills") {
      if (not busy()) root_agent.reload_skills();
      const agent::SkillCatalog& catalog = root_agent.skill_catalog();
      std::string msg;
      for (const agent::SkillInfo& skill : catalog.skills) {
        msg += (skill.command ? "/" : "  ") + skill.name + " — " +
               skill.description + "\n";
      }
      for (const std::string& note : catalog.notes) msg += "(" + note + ")\n";
      push_notice(TranscriptNode::Kind::Notice,
                  msg.empty() ? "no skills found in the skills directory" : msg);
      return;
    }

    if (entered == "/mcp" or entered.rfind("/mcp ", 0) == 0) {
      handle_mcp(entered);
      return;
    }

    // /mcp__<server>__<prompt> [args]: a server's prompt as the next user turn.
    if (entered.rfind("/mcp__", 0) == 0 and registry != nullptr) {
      std::optional<m8::PromptCommand> command =
          m8::parse_prompt_command(entered, *registry->snapshot());
      if (command) {
        if (not command->error.empty()) {
          push_notice(TranscriptNode::Kind::Notice, command->error);
          return;
        }
        if (busy()) {
          push_notice(TranscriptNode::Kind::Notice,
                      "the agent is working; wait for it to finish");
          return;
        }
        // prompts/get is a round trip to the server, so it runs on the turn's
        // thread, where the busy flag covers it.
        start_turn(entered, std::string(),
                   [registry, command = std::move(*command)](std::string& objective) {
                     const mcp::CallOutcome outcome = registry->get_prompt(
                         command.server, command.prompt, command.arguments,
                         mcp::CallContext{});
                     if (not outcome.ok) {
                       objective = "mcp: " + command.server + "'s prompt " +
                                   command.prompt + " failed: " + outcome.error;
                       return false;
                     }
                     objective = mcp::render_prompt_messages(outcome.result);
                     if (objective.empty()) {
                       objective = "mcp: " + command.server + "'s prompt " +
                                   command.prompt + " came back empty";
                       return false;
                     }
                     return true;
                   });
        return;
      }
    }

    // /<name> [args] for a skill whose frontmatter opted in with command: true.
    if (entered.size() > 1 and entered[0] == '/') {
      const size_t space = entered.find(' ');
      const std::string cmd = entered.substr(
          1, space == std::string::npos ? std::string::npos : space - 1);
      const std::string cmd_args =
          space == std::string::npos ? "" : entered.substr(space + 1);
      const agent::SkillInfo* skill = root_agent.skill_catalog().find(cmd);
      if (skill != nullptr and skill->command) {
        if (not skill->argument_hint.empty() and cmd_args.empty()) {
          push_notice(TranscriptNode::Kind::Notice, "/" + cmd + "  " +
                                                        skill->argument_hint +
                                                        "\n" + skill->description);
          return;
        }
        if (busy()) {
          push_notice(TranscriptNode::Kind::Notice,
                      "the agent is working; wait for it to finish");
          return;
        }
        std::string objective = "Run the \"" + cmd +
                                "\" skill: use the `skill` tool to load it, then "
                                "follow its SKILL.md instructions.";
        if (not cmd_args.empty()) objective += "  Arguments: " + cmd_args;
        start_turn(entered, std::move(objective));
        return;
      }
    }

    // Two turns at once would share one transcript. The draft stays in the
    // input, to send when the agent is done.
    if (busy()) {
      input_value = entered;
      input_cursor = static_cast<int>(entered.size());
      push_notice(TranscriptNode::Kind::Notice,
                  "the agent is working; send this when it is done");
      return;
    }
    start_turn(entered, entered);
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

  // The answer box: a second input that has the keyboard while a question
  // waits. Its own text and no history, so an answer never lands in the
  // message draft or in Up-arrow recall.
  std::string answer_value;
  int answer_cursor = 0;
  f::InputOption answer_option;
  answer_option.content = &answer_value;
  answer_option.cursor_position = &answer_cursor;
  answer_option.placeholder = "your answer";
  answer_option.transform = [](f::InputState state) {
    f::Element element = std::move(state.element);
    if (state.is_placeholder) element |= f::dim;
    return element | f::color(f::Color::White) | f::bgcolor(f::Color::Black);
  };
  auto answer_input = f::Input(answer_option);
  int input_tab = 0;  // 0: the message box, 1: the answer box
  auto inputs = f::Container::Tab({input, answer_input}, &input_tab);

  // Follows the queue; the UI thread is the only one that touches input_tab
  // or the answer box. A new question resets the box to its starting text.
  uint64_t answering = 0;  // the question in the answer box; 0 for none
  auto sync_answer_mode = [&]() -> std::optional<agentui::InteractionQueue::Shown> {
    std::optional<agentui::InteractionQueue::Shown> shown = interactions.current();
    const uint64_t id = shown ? shown->id : 0;
    if (id != answering) {
      answering = id;
      answer_value = shown ? shown->question.initial : std::string();
      answer_cursor = static_cast<int>(answer_value.size());
    }
    input_tab = shown ? 1 : 0;
    return shown;
  };

  auto root = f::Renderer(inputs, [&] {
    const std::optional<agentui::InteractionQueue::Shown> asking = sync_answer_mode();
    // Before taking `mutex`: the registry's own lock is never taken under it.
    const std::string mcp_tag =
        registry != nullptr ? m8::mcp_header(*registry->snapshot()) : std::string();
    std::vector<f::Element> lines;
    std::vector<f::Element> tiles;
    float current_scroll_y;
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
          if (it != subagent_nodes.end()) tiles.push_back(render_pane(*it->second));
        }
      } else {
        for (TranscriptNode& node : transcript) render_node(lines, node, 0);
        if (waiting_for_reply) {
          lines.push_back(f::text("agent is working...") | f::dim);
        }
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
    std::string sub_part;
    if (running_n > 0) {
      sub_part = "  |  subagents: " + std::to_string(running_n) + "/" +
                 std::to_string(max_agents) +
                 (transcript_forced ? "  (Ctrl+G: grid)"
                                    : "  (Ctrl+G: transcript)");
    }

    f::Element middle;
    if (show_grid) {
      const int shown = static_cast<int>(tiles.size());
      const GridShape gs = grid_shape(std::max(shown, 1));
      std::vector<f::Elements> matrix;
      matrix.reserve(gs.rows);
      for (int r = 0; r < gs.rows; ++r) {
        f::Elements row;
        row.reserve(gs.cols);
        for (int c = 0; c < gs.cols; ++c) {
          const int idx = r * gs.cols + c;  // row-major fill
          row.push_back(idx < shown ? std::move(tiles[idx]) : empty_pane());
        }
        matrix.push_back(std::move(row));
      }
      middle = f::gridbox(std::move(matrix)) | f::flex;
    } else {
      middle = f::vbox(lines) |
               f::focusPositionRelative(0.f, current_scroll_y) |
               f::vscroll_indicator | f::yframe | f::flex |
               f::reflect(viewport_box);
    }

    f::Elements page = {
        f::text("m8  |  model: " + model + "  |  policy: " + pol.name() + ctx_part +
                (mcp_tag.empty() ? "" : "  |  " + mcp_tag) + sub_part) |
            f::bold | f::center,
        f::separator(),
        std::move(middle),
        f::separator(),
    };
    if (asking) {
      page.push_back(render_question(asking->question));
      page.push_back(answer_input->Render() | f::color(f::Color::White) |
                     f::bgcolor(f::Color::Black) | f::borderStyled(f::Color::Yellow));
    } else {
      page.push_back(input->Render() | f::color(f::Color::White) |
                     f::bgcolor(f::Color::Black) | f::border);
    }
    return f::vbox(std::move(page)) | f::border;
  });

  root = f::CatchEvent(root, [&](f::Event event) {
    constexpr float kWheelStep = 0.1f;
    constexpr float kPageStep = 0.3f;
    static const f::Event kAltEnterCR = f::Event::Special("\x1b\r");
    static const f::Event kAltEnterLF = f::Event::Special("\x1b\n");
    static const f::Event kShiftEnterCsiU = f::Event::Special("\x1b[13;2u");
    static const f::Event kShiftEnterLegacy =
        f::Event::Special("\x1b[27;2;13~");

    // True when the pane grid, not the transcript, owns the middle region.
    // Call only while holding `mutex`.
    auto grid_active = [&] {
      return not running_agents.empty() and not force_conversation_view;
    };

    // A question waiting: Return answers it, Ctrl+D declines it, Esc cancels
    // it, and the message box's keys (history, newlines) are off. Scrolling
    // and the view keys below still work.
    if (const std::optional<agentui::InteractionQueue::Shown> asking = sync_answer_mode()) {
      if (event == f::Event::Return) {
        interactions.answer(asking->id, {agentui::Answer::Kind::Answered, answer_value});
        return true;
      }
      if (event == f::Event::CtrlD) {
        interactions.answer(asking->id, {agentui::Answer::Kind::Declined, std::string()});
        return true;
      }
      if (event == f::Event::Escape) {
        interactions.answer(asking->id, {agentui::Answer::Kind::Cancelled, std::string()});
        return true;
      }
      if (event == f::Event::ArrowUp or event == f::Event::ArrowDown or
          event == kAltEnterCR or event == kAltEnterLF or event == kShiftEnterCsiU or
          event == kShiftEnterLegacy) {
        return true;
      }
    }

    if (event == f::Event::CtrlG) {
      std::lock_guard<std::mutex> lock(mutex);
      // No grid without subagents; swallow the key anyway so it never reaches
      // the input.
      if (not running_agents.empty()) {
        force_conversation_view = not force_conversation_view;
      }
      return true;
    }
    if (event == f::Event::PageUp) {
      std::lock_guard<std::mutex> lock(mutex);
      if (grid_active()) return true;
      scroll_y = std::clamp(scroll_y - kPageStep, 0.f, 1.f);
      return true;
    }
    if (event == f::Event::PageDown) {
      std::lock_guard<std::mutex> lock(mutex);
      if (grid_active()) return true;
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
        // beyond the frame still has a box, and it must not answer clicks. In
        // grid mode the transcript isn't drawn at all.
        if (not grid_active() and viewport_box.Contain(mouse.x, mouse.y)) {
          for (TranscriptNode& node : transcript) {
            if (hit_test(node, mouse.x, mouse.y)) return true;
          }
        }
        // Fall through: a click elsewhere still belongs to the input.
      }
      if (event.mouse().button == f::Mouse::WheelUp) {
        std::lock_guard<std::mutex> lock(mutex);
        if (grid_active()) return true;
        scroll_y = std::clamp(scroll_y - kWheelStep, 0.f, 1.f);
        return true;
      }
      if (event.mouse().button == f::Mouse::WheelDown) {
        std::lock_guard<std::mutex> lock(mutex);
        if (grid_active()) return true;
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
  // Nobody is left to answer: every question waiting (and every one asked
  // from here on) comes back cancelled, so no thread stays blocked on one.
  interactions.cancel_all();
  // Stop the MCP servers next: a call still in flight fails now, and the
  // agent thread making it unwinds while the observer below still ignores it.
  // Every server's process group is gone when this returns.
  if (registry != nullptr) registry->shutdown();
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
