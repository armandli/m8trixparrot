#include <core/agent/agent.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <core/agent/agent_pool.h>
#include <core/vdb/memory_store.h>
#include <core/agent/system_prompt.h>
#include <core/mcp/resource_tool.h>
#include <core/mcp/tool_search.h>
#include <core/tools/tools_util.h>
#include <core/util/json_value.h>

namespace agent {

namespace {

// A tool result longer than this is clipped before it goes back to the model.
// The tools already cap themselves at 100KB, which is still far more than a
// step's worth of context is worth spending.
constexpr size_t kMaxToolResultBytes = 16000;

// tool_search calls one turn may make before the model is told to work with
// what it has: a model that cannot find a tool tends to search in circles.
constexpr int kMaxToolSearchCalls = 8;

// The loaded-tool budget, applied between turns: least recently used first.
constexpr size_t kMaxLoadedMcpTools = 40;

// A subagent starts with at most this many of its parent's loaded tools.
constexpr size_t kMaxInheritedMcpTools = 20;

// Every name dispatch() can route, advertised or not. A call by one of these
// is never reinterpreted as a shortened MCP tool name.
bool is_builtin_tool(std::string_view name) {
  static constexpr const char* kBuiltins[] = {
      "bash",   "bash_repl", "read",      "write",           "edit",
      "websearch", "bash_search", "memory", "ask_user",       "skill",
      "subagent_create", "subagent_wait", "tool_search", "mcp_resource"};
  for (const char* builtin : kBuiltins) {
    if (name == builtin) return true;
  }
  return false;
}

// A one-line rendering of the arguments that matter for display, so the UI can
// show "grep pattern=\"teh\"" rather than the whole JSON object.
std::string summarize(const std::string& tool_name, const tools::ToolArgs& args) {
  static const char* kInteresting[] = {"command",   "path",   "pattern",
                                       "query",     "url",    "content",
                                       "objective", "id",     "action",
                                       "name",      "prompt"};

  std::string summary;
  for (const char* key : kInteresting) {
    const std::optional<std::string> value = tools::string_arg(args, key);
    if (not value) continue;
    if (not summary.empty()) summary += "  ";
    summary += std::string(key) + "=";
    // Newlines would break the one-line promise; a long value is elided.
    std::string shown = value->substr(0, 60);
    std::replace(shown.begin(), shown.end(), '\n', ' ');
    summary += "\"" + shown + (value->size() > 60 ? "..." : "") + "\"";
  }

  if (summary.empty()) return tool_name;
  return summary;
}

// For tools whose arguments have none of the names above (most MCP tools): the
// first two scalar arguments, in the order the model wrote them.
std::string summarize(const oc::ToolCall& call, const tools::ToolArgs& args) {
  const std::string known = summarize(call.name, args);
  if (known != call.name) return known;
  const std::optional<util::JsonValue> parsed = util::JsonValue::parse(call.arguments);
  if (not parsed or not parsed->is_object()) return call.name;
  std::string summary;
  int shown = 0;
  for (const util::JsonValue::Member& member : parsed->members()) {
    const util::JsonValue& value = member.value;
    if (value.is_object() or value.is_array() or value.is_null()) continue;
    std::string text = value.is_string() ? value.as_string() : value.dump();
    std::replace(text.begin(), text.end(), '\n', ' ');
    if (not summary.empty()) summary += "  ";
    summary += member.key + "=\"" + text.substr(0, 60) +
               (text.size() > 60 ? "..." : "") + "\"";
    if (++shown == 2) break;
  }
  return summary.empty() ? call.name : summary;
}

bool rejected_by_ollama(const std::string& error) {
  return error.rfind("ollama returned HTTP 400", 0) == 0;
}

// The transcript flattened to labelled text, for the summarizer to read. A
// message that is loaded skill content is reduced to a placeholder — the model
// can reload the skill after summarizing, so its body needn't be re-digested.
std::string render_transcript(const std::vector<oc::ChatMessage>& transcript) {
  std::ostringstream out;
  for (const oc::ChatMessage& message : transcript) {
    if (not message.skill_label.empty()) {
      out << "[skill '" << message.skill_label
          << "' content was loaded here]\n\n";
      continue;
    }
    out << "[" << message.role;
    if (not message.tool_name.empty()) out << " " << message.tool_name;
    out << "]\n";
    if (not message.content.empty()) out << message.content << "\n";
    for (const oc::ToolCall& call : message.tool_calls) {
      out << "-> called " << call.name << "(" << call.arguments << ")\n";
    }
    out << "\n";
  }
  return out.str();
}

// "128k" / "1.5M" / "640".
std::string human_tokens(int64_t n) {
  if (n < 1000) return std::to_string(n);
  if (n < 1000000) return std::to_string((n + 500) / 1000) + "k";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1000000.0);
  return std::string(buf);
}

}  // namespace

Agent::Agent(AgentOptions options, const policy::PolicyInterface& pol, std::string id,
             std::string parent_id, int depth)
    : mOptions(std::move(options)),
      mPolicy(pol),
      mStore(mOptions.session_dir),
      mId(std::move(id)),
      mParentId(std::move(parent_id)),
      mDepth(depth),
      mLabel(depth == 0 ? "root" : "subagent") {
  if (not mOptions.mcp_initial_tools.empty()) {
    mLoadedMcpTools.assign(mOptions.mcp_initial_tools);
  }
}

bool Agent::skills_offered() const {
  return mOptions.enable_skills and not catalog().skills.empty();
}

bool Agent::ask_user_offered() const {
  return static_cast<bool>(mOptions.ask_user_handler);
}

std::vector<Agent::ToolEntry> Agent::builtin_tools() const {
  std::vector<ToolEntry> entries;
  if (mOptions.enable_bash_repl) {
    entries.push_back({"bash_repl", tools::BashReplTool::description()});
  } else {
    entries.push_back({"bash", tools::BashTool().description()});
  }
  if (mOptions.enable_file_tools) {
    entries.push_back({"read", tools::ReadTool().description()});
    entries.push_back({"write", tools::WriteTool().description()});
    entries.push_back({"edit", tools::EditTool().description()});
  }
  if (mOptions.enable_web_search) {
    entries.push_back({"websearch", tools::WebSearchTool().description()});
  }
  if (mOptions.enable_bash_search) {
    entries.push_back({"bash_search", tools::BashSearchTool().description()});
  }
  if (mOptions.enable_memory) {
    entries.push_back({"memory", vdb::MemoryTool::description()});
  }
  if (skills_offered()) entries.push_back({"skill", SkillTool::description()});
  if (mOptions.enable_subagents) {
    entries.push_back({"subagent_create", SubagentCreateTool::description()});
    entries.push_back({"subagent_wait", SubagentWaitTool::description()});
  }
  if (ask_user_offered()) {
    entries.push_back(
        {"ask_user", tools::AskUserTool{mOptions.ask_user_handler}.description()});
  }
  return entries;
}

bool Agent::mcp_would_defer(const mcp::Catalog& catalog) const {
  int64_t tokens = 0;
  size_t count = 0;
  for (const mcp::CatalogTool& tool : catalog.tools) {
    if (tool.always_load) continue;
    tokens += tool.tokens;
    ++count;
  }
  return mOptions.tool_search.defers(tokens, count, mOptions.context_window_tokens);
}

Agent::StepTools Agent::step_tools() const {
  StepTools step;
  for (const ToolEntry& entry : builtin_tools()) {
    step.names.push_back(entry.name);
    step.schemas.push_back(entry.schema);
  }
  // After Ollama refused this turn's MCP schemas, the turn goes on without
  // them rather than failing every remaining step the same way.
  if (not mOptions.mcp or mMcpSchemasFailed) return step;

  step.catalog = mTurnCatalog ? mTurnCatalog : mOptions.mcp->snapshot();
  const mcp::Catalog& catalog = *step.catalog;
  step.deferred = mMcpDeferred.load() or mcp_would_defer(catalog);
  step.resources = catalog.any_resources();

  // Stable parts first (tool_search, mcp_resource, always-loaded tools), the
  // growing loaded set last: what is already in Ollama's prompt cache stays
  // put as tools are added.
  std::vector<const mcp::CatalogTool*> in_array;
  if (step.deferred) {
    bool anything_deferred = false;
    for (const mcp::CatalogTool& tool : catalog.tools) {
      if (not tool.always_load and not mLoadedMcpTools.contains(tool.exposed)) {
        anything_deferred = true;
        break;
      }
    }
    step.tool_search = anything_deferred or catalog.any_connecting();
    if (step.tool_search) {
      step.names.push_back("tool_search");
      step.schemas.push_back(mcp::ToolSearchTool::description());
      ++step.mcp_schemas;
    }
    if (step.resources) {
      step.names.push_back("mcp_resource");
      step.schemas.push_back(mcp::ResourceTool::description());
      ++step.mcp_schemas;
    }
    for (const mcp::CatalogTool& tool : catalog.tools) {
      if (tool.always_load) in_array.push_back(&tool);
    }
    for (const std::string& name : mLoadedMcpTools.names()) {
      const mcp::CatalogTool* tool = catalog.find(name);
      if (tool == nullptr or tool->always_load) continue;
      in_array.push_back(tool);
      step.loaded.push_back(name);
    }
  } else {
    if (step.resources) {
      step.names.push_back("mcp_resource");
      step.schemas.push_back(mcp::ResourceTool::description());
      ++step.mcp_schemas;
    }
    for (const mcp::CatalogTool& tool : catalog.tools) in_array.push_back(&tool);
  }
  for (const mcp::CatalogTool* tool : in_array) step.schemas.push_back(tool->schema_json);
  step.mcp_tools = in_array.size();
  step.mcp_schemas += in_array.size();
  return step;
}

std::vector<std::string> Agent::tool_schemas() const { return step_tools().schemas; }

std::vector<std::string> Agent::tool_names() const { return step_tools().names; }

const SkillCatalog& Agent::catalog() const {
  if (not mCatalog) mCatalog = SkillCatalog::discover(mOptions.skills_dir);
  return *mCatalog;
}

const SkillCatalog& Agent::skill_catalog() const { return catalog(); }

void Agent::reload_skills() {
  mCatalog = SkillCatalog::discover(mOptions.skills_dir);
}

std::string Agent::skill_label_for(const oc::ToolCall& call, const tools::ToolArgs& args,
                                   const tools::ToolResult& result) const {
  if (not mOptions.enable_skills) return std::string();
  if (call.name == "skill") {
    if (not result.ok) return std::string();
    const std::optional<std::string> action = tools::string_arg(args, "action");
    const std::optional<std::string> name = tools::string_arg(args, "name");
    if (action and name and *action == "load") return *name;
    return std::string();
  }
  // A shell command that reads out of a skill's directory is how the model gets
  // at a skill's supporting files, so the resulting message is tagged with that
  // skill and `skill unload` can find it later.
  if (call.name == "bash_repl" or call.name == "bash") {
    return catalog().label_for_text(call.arguments);
  }
  return std::string();
}

tools::BashReplSession& Agent::shell() {
  if (not mShell) mShell = std::make_unique<tools::BashReplSession>();
  return *mShell;
}

void Agent::reset() {
  mTranscript.clear();
  mSessionId.clear();
  mContextTokens.store(0);
  mCatalog.reset();  // pick up skills added since the last scan
  mShell.reset();    // a new conversation gets a clean shell, not the old one
  mLoadedMcpTools.clear();
  mMcpDeferred.store(false);
  mTurnCatalog.reset();
}

int64_t Agent::summarize_threshold() const {
  const int64_t hard = mOptions.context_summarize_at_tokens;
  if (mOptions.context_window_tokens <= 0) return hard;
  const int64_t soft =
      static_cast<int64_t>(mOptions.context_window_tokens) * 4 / 5;
  return std::min(hard, soft);
}

int64_t Agent::context_limit() const { return summarize_threshold(); }

int64_t Agent::context_tokens() const { return mContextTokens.load(); }

void Agent::emit_context_usage() const {
  AgentEvent event;
  event.kind = AgentEvent::Kind::ContextUsage;
  event.tokens = mContextTokens.load();
  event.token_budget = summarize_threshold();
  emit(event);
}

void Agent::maybe_summarize_context() {
  if (mTranscript.size() < 2) return;

  const int64_t current =
      std::max(mContextTokens.load(), estimate_transcript_tokens(mTranscript));
  if (current < summarize_threshold()) return;

  emit({AgentEvent::Kind::Notice,
        "context at ~" + human_tokens(current) +
            " tokens; summarizing before continuing",
        "", ""});

  // Skills loaded before the summary lose their body (render_transcript drops
  // it); tell the model which they were so it can reload any it still needs.
  std::vector<std::string> loaded;
  for (const oc::ChatMessage& message : mTranscript) {
    if (not message.skill_label.empty() and
        std::find(loaded.begin(), loaded.end(), message.skill_label) ==
            loaded.end()) {
      loaded.push_back(message.skill_label);
    }
  }

  std::vector<oc::ChatMessage> request;
  const std::string summary_prompt =
      mOptions.summary_system_prompt.empty()
          ? default_summary_prompt()
          : mOptions.summary_system_prompt;
  request.push_back(oc::ChatMessage{"system", summary_prompt, {}, ""});
  request.push_back(oc::ChatMessage{"user", render_transcript(mTranscript), {}, ""});

  const uint64_t ticket = oc::OllamaClient::instance().enqueue_chat(request, {});
  const oc::ChatResult reply = oc::OllamaClient::instance().wait_for(ticket);

  if (not reply.ok or reply.content.empty()) {
    emit({AgentEvent::Kind::Notice,
          "context summarization failed (" +
              (reply.error.empty() ? std::string("empty response")
                                   : reply.error) +
              "); continuing",
          "", ""});
    return;
  }

  std::string seed =
      "The earlier conversation was summarized to save context. Summary:\n\n" +
      reply.content + "\n\nContinue the task from here.";
  if (not loaded.empty()) {
    seed += "\n\nSkills loaded before this summary:";
    for (const std::string& name : loaded) seed += " " + name;
    seed += ". Reload any you still need with the `skill` tool.";
  }
  // Loaded MCP tools live in the tools array, not the transcript, so they
  // survive the summary; say which they are, since the search results that
  // introduced them are gone.
  if (const std::vector<std::string> tools = mLoadedMcpTools.names();
      mOptions.mcp and not tools.empty()) {
    seed += "\n\nMCP tools still loaded:";
    for (const std::string& name : tools) seed += " " + name;
    seed += ".";
  }

  mTranscript.clear();
  mTranscript.push_back(oc::ChatMessage{"user", seed, {}, ""});
  mContextTokens.store(estimate_transcript_tokens(mTranscript));

  AgentEvent summarized;
  summarized.kind = AgentEvent::Kind::ContextSummarized;
  summarized.text = "context summarized (~" + human_tokens(current) +
                    " tokens folded into a summary)";
  summarized.tokens = current;
  emit(summarized);
}

PromptFacts Agent::prompt_facts(const StepTools& step) const {
  PromptFacts facts;
  facts.depth = mDepth;
  facts.max_depth = mOptions.max_depth;
  facts.max_agents = mOptions.max_agents;
  facts.free_agent_slots =
      std::max(0, mOptions.max_agents - AgentPool::instance().live_count());
  facts.tool_names = step.names;

  facts.enable_bash_repl = mOptions.enable_bash_repl;
  facts.enable_file_tools = mOptions.enable_file_tools;
  facts.enable_web_search = mOptions.enable_web_search;
  facts.enable_bash_search = mOptions.enable_bash_search;
  facts.enable_memory = mOptions.enable_memory;
  facts.enable_subagents = mOptions.enable_subagents;
  facts.ask_user_offered = ask_user_offered();
  facts.can_spawn_subagents =
      mOptions.enable_subagents and mDepth < mOptions.max_depth;

  // Only when the `skill` tool is actually advertised: a catalog the model was
  // given no way to load is not a fact about its situation.
  if (skills_offered()) facts.skills = &catalog();

  facts.mcp.catalog = step.catalog.get();
  facts.mcp.deferred = step.deferred;
  facts.mcp.tool_search_offered = step.tool_search;
  facts.mcp.resources_offered = step.resources;
  facts.mcp.loaded = step.loaded;
  facts.mcp.tools_in_array = step.mcp_tools;

  return facts;
}

PromptFacts Agent::prompt_facts() const { return prompt_facts(step_tools()); }

std::string Agent::system_prompt(const StepTools& step) const {
  // The facts borrow the step's catalog, which lives as long as `step`.
  const PromptFacts facts = prompt_facts(step);
  return mOptions.system_prompt_builder
             ? mOptions.system_prompt_builder(facts)
             : default_system_prompt(facts);
}

std::string Agent::system_prompt() const { return system_prompt(step_tools()); }

tools::ToolResult Agent::dispatch(const std::string& tool_name, const tools::ToolArgs& args) {
  if (mOptions.enable_bash_repl and tool_name == "bash_repl")
    return tools::BashReplTool{shell()}.execute(args);
  if (not mOptions.enable_bash_repl and tool_name == "bash")
    return tools::BashTool().execute(args);
  if (mOptions.enable_file_tools and tool_name == "read")
    return tools::ReadTool().execute(args);
  if (mOptions.enable_file_tools and tool_name == "write")
    return tools::WriteTool().execute(args);
  if (mOptions.enable_file_tools and tool_name == "edit")
    return tools::EditTool().execute(args);
  if (mOptions.enable_web_search and tool_name == "websearch")
    return tools::WebSearchTool().execute(args);
  if (mOptions.enable_bash_search and tool_name == "bash_search")
    return tools::BashSearchTool().execute(args);
  if (mOptions.enable_memory and tool_name == "memory") {
    vdb::MemoryOptions memory;
    memory.path = mOptions.memory_path;
    memory.embed_model = mOptions.memory_embed_model;
    return vdb::MemoryTool{std::move(memory)}.execute(args);
  }
  if (ask_user_offered() and tool_name == "ask_user")
    return tools::AskUserTool{mOptions.ask_user_handler}.execute(args);
  if (mOptions.enable_skills and tool_name == "skill")
    return SkillTool{mTranscript, mContextTokens, catalog()}.execute(args);
  if (mOptions.enable_subagents and tool_name == "subagent_create") {
    // The child starts with the parent's most recently loaded MCP tools.
    AgentOptions child = mOptions;
    if (mOptions.mcp) {
      std::vector<std::string> loaded = mLoadedMcpTools.names();
      if (loaded.size() > kMaxInheritedMcpTools) {
        loaded.erase(loaded.begin(), loaded.end() - kMaxInheritedMcpTools);
      }
      child.mcp_initial_tools = std::move(loaded);
    }
    return SubagentCreateTool{mId, mPolicy, child}.execute(args);
  }
  if (mOptions.enable_subagents and tool_name == "subagent_wait")
    return SubagentWaitTool{}.execute(args);

  tools::ToolResult unknown;
  unknown.error = "no tool named '" + tool_name +
                  "' exists; call one of the tools you were given";
  if (mOptions.mcp) unknown.error += ", or find MCP tools with tool_search";
  return unknown;
}

mcp::CallContext Agent::mcp_context() const {
  mcp::CallContext context;
  context.agent_id = mId;
  context.agent_label = mDepth == 0 ? mLabel : mLabel + " " + mId.substr(0, 8);
  return context;
}

void Agent::begin_mcp_turn() {
  mToolSearchCalls = 0;
  mMcpSchemasFailed = false;
  if (not mOptions.mcp) return;
  mTurnCatalog = mOptions.mcp->snapshot();

  // Between turns only: a tool is never taken away in the middle of one.
  const int64_t budget = mOptions.context_window_tokens > 0
                             ? static_cast<int64_t>(mOptions.context_window_tokens) / 5
                             : 8000;
  const std::vector<std::string> dropped =
      mLoadedMcpTools.evict(kMaxLoadedMcpTools, budget, *mTurnCatalog);
  if (not dropped.empty()) {
    std::string text = "unloaded " + std::to_string(dropped.size()) +
                       " MCP tool(s) not used recently:";
    for (const std::string& name : dropped) text += " " + name;
    emit({AgentEvent::Kind::Notice, text, "", ""});
  }
  // Sticky: once an agent's MCP tools are deferred they stay deferred, so the
  // tool set does not jump around as late servers connect.
  if (not mMcpDeferred.load() and mcp_would_defer(*mTurnCatalog)) {
    mMcpDeferred.store(true);
  }
}

tools::ToolResult Agent::run_tool_search(const tools::ToolArgs& args) {
  if (++mToolSearchCalls > kMaxToolSearchCalls) {
    tools::ToolResult refused;
    refused.error = "tool_search was already called " +
                    std::to_string(kMaxToolSearchCalls) +
                    " times this turn; use the tools you have loaded, or tell "
                    "the user what is missing";
    return refused;
  }
  std::shared_ptr<const mcp::Catalog> catalog =
      mTurnCatalog ? mTurnCatalog : mOptions.mcp->snapshot();
  if (catalog->any_connecting()) {
    // A server a few seconds from ready is worth waiting for; its tools are
    // what the model may be about to look for.
    catalog = mOptions.mcp->wait_until_settled(std::chrono::seconds(5));
    mTurnCatalog = catalog;
  }
  return mcp::ToolSearchTool{*catalog, mLoadedMcpTools}.execute(args);
}

tools::ToolResult Agent::run_mcp_tool(const std::string& exposed,
                                      const std::string& raw_arguments) {
  const std::shared_ptr<const mcp::Catalog> catalog =
      mTurnCatalog ? mTurnCatalog : mOptions.mcp->snapshot();
  const mcp::CatalogTool* tool = catalog->find(exposed);
  // Called by name without loading it first: run it, and load it so the next
  // call has its schema.
  if (tool != nullptr and mMcpDeferred.load() and not tool->always_load and
      mLoadedMcpTools.add(exposed)) {
    emit({AgentEvent::Kind::Notice, "loaded " + exposed + " on first use", "", ""});
  }
  mLoadedMcpTools.touch(exposed);
  return mOptions.mcp->call_tool(exposed, raw_arguments, mcp_context());
}

tools::ToolResult Agent::dispatch_call(const oc::ToolCall& call,
                                       const tools::ToolArgs& args) {
  if (mOptions.mcp) {
    if (call.name == "tool_search") return run_tool_search(args);
    if (call.name == "mcp_resource") {
      const mcp::CallContext context = mcp_context();
      return mcp::ResourceTool{*mOptions.mcp, context}.execute(args);
    }
    if (mcp::is_mcp_tool_name(call.name)) {
      return run_mcp_tool(call.name, call.arguments);
    }
    if (not is_builtin_tool(call.name)) {
      // A model that saw "create_issue" in the catalog and called it bare.
      const std::shared_ptr<const mcp::Catalog> catalog =
          mTurnCatalog ? mTurnCatalog : mOptions.mcp->snapshot();
      const mcp::NameResolution resolved = mcp::resolve_tool_name(*catalog, call.name);
      if (resolved.tool != nullptr) {
        return run_mcp_tool(resolved.tool->exposed, call.arguments);
      }
    }
  }
  return dispatch(call.name, args);
}

SessionResult Agent::resume(const std::string& session_id) {
  // Sessions hold only the result tree, so there is no transcript to restore
  // and mSessionId stays empty — a continued conversation opens a new file.
  return session_id.empty() ? mStore.latest() : mStore.load(session_id);
}

SessionStoreResult Agent::save() const {
  if (mDepth != 0) {
    SessionStoreResult skipped;
    skipped.ok = true;
    return skipped;
  }
  return mStore.store(AgentPool::instance().assemble_tree(mId), mSessionId);
}

void Agent::emit(AgentEvent event) const {
  event.agent_id = mId;
  event.parent_id = mParentId;
  event.depth = mDepth;
  event.agent_label = mLabel;
  AgentPool::instance().emit(event);
}

AgentResult Agent::run_turn(const std::string& objective) {
  AgentResult self;
  self.objective = objective;

  mTranscript.push_back(oc::ChatMessage{"user", objective, {}, ""});

  begin_mcp_turn();
  // The pinned catalog is for this turn only; drop it however the turn ends.
  struct TurnEnd {
    std::shared_ptr<const mcp::Catalog>& catalog;
    ~TurnEnd() { catalog.reset(); }
  } turn_end{mTurnCatalog};

  for (int step = 0; step < mOptions.max_steps; ++step) {
    self.steps = step + 1;

    // Compact the transcript before it can overflow the context window. This
    // may itself run an Ollama call and replace mTranscript with a summary.
    maybe_summarize_context();

    // Tools and the system message are rebuilt every step rather than stored:
    // a skill load/unload, or an MCP tool tool_search just loaded, lands in
    // the very next call. One StepTools serves both, so they always agree.
    StepTools offered = step_tools();
    std::vector<oc::ChatMessage> messages;
    messages.reserve(mTranscript.size() + 1);
    messages.push_back(oc::ChatMessage{"system", system_prompt(offered), {}, ""});
    messages.insert(messages.end(), mTranscript.begin(), mTranscript.end());

    uint64_t ticket =
        oc::OllamaClient::instance().enqueue_chat(messages, offered.schemas);
    oc::ChatResult reply = oc::OllamaClient::instance().wait_for(ticket);

    // An MCP schema Ollama cannot take fails the whole request. Retry without
    // them so the turn survives; lowering should prevent this, and the notice
    // says so if it ever does not.
    if (not reply.ok and offered.mcp_schemas > 0 and rejected_by_ollama(reply.error)) {
      mMcpSchemasFailed = true;
      emit({AgentEvent::Kind::Notice,
            "Ollama rejected this request's MCP tool schemas; continuing this "
            "turn without them (" + reply.error.substr(0, 200) + ")",
            "", ""});
      offered = step_tools();
      messages.front() = oc::ChatMessage{"system", system_prompt(offered), {}, ""};
      ticket = oc::OllamaClient::instance().enqueue_chat(messages, offered.schemas);
      reply = oc::OllamaClient::instance().wait_for(ticket);
    }
    if (not reply.ok) {
      self.ok = false;
      self.error = reply.error;
      emit({AgentEvent::Kind::Error, reply.error, "", ""});
      AgentPool::instance().set_result(mId, self);
      if (mDepth == 0) save();
      return self;
    }

    mContextTokens.store(reply.prompt_eval_count);
    emit_context_usage();

    mTranscript.push_back(
        oc::ChatMessage{"assistant", reply.content, reply.tool_calls, ""});

    if (not reply.content.empty()) {
      emit({AgentEvent::Kind::Assistant, reply.content, "", ""});
    }

    // No tool calls means the model is answering, which ends the turn.
    if (reply.tool_calls.empty()) {
      self.ok = true;
      self.conclusion = reply.content;
      AgentPool::instance().set_result(mId, self);
      if (mDepth == 0) {
        const SessionStoreResult saved = save();
        if (not saved.ok) {
          emit({AgentEvent::Kind::Notice,
                "failed to save session: " + saved.error, "", ""});
        } else {
          mSessionId = saved.session_id;
        }
      }
      return self;
    }

    for (const oc::ToolCall& call : reply.tool_calls) {
      std::string parse_error;
      const tools::ToolArgs args = tools::args_from_json(call.arguments, parse_error);
      const std::string summary = summarize(call, args);

      emit({AgentEvent::Kind::ToolCall, "", call.name, summary});

      if (not parse_error.empty()) {
        const std::string message =
            "could not read the arguments for '" + call.name + "': " +
            parse_error;
        emit({AgentEvent::Kind::ToolResult, message, call.name, ""});
        mTranscript.push_back(oc::ChatMessage{"tool", message, {}, call.name});
        continue;
      }

      const policy::PolicyResult verdict = mPolicy.verify(call.name, args);
      if (not verdict.allowed()) {
        // The refusal goes back as the tool's result: the model is told why and
        // can pick another approach, which is the whole point of making
        // policies explain themselves.
        emit({AgentEvent::Kind::Denied, verdict.reason, call.name, summary});
        mTranscript.push_back(
            oc::ChatMessage{"tool", verdict.reason, {}, call.name});
        continue;
      }

      const tools::ToolResult executed = dispatch_call(call, args);
      std::string content = executed.ok ? executed.output : executed.error;
      // A tool can legitimately produce nothing (ls of an empty directory,
      // grep with no hits). Saying so beats sending an empty message.
      if (content.empty()) content = "[no output]";

      emit({AgentEvent::Kind::ToolResult, content, call.name, ""});
      oc::ChatMessage tool_message{"tool",
                               tools::clip_text(content, kMaxToolResultBytes),
                               {}, call.name};
      tool_message.skill_label = skill_label_for(call, args, executed);
      mTranscript.push_back(std::move(tool_message));
    }
  }

  self.ok = false;
  self.hit_step_limit = true;
  self.error = "gave up after " + std::to_string(mOptions.max_steps) +
               " steps without a final answer";
  emit({AgentEvent::Kind::Notice, self.error, "", ""});
  AgentPool::instance().set_result(mId, self);
  if (mDepth == 0) {
    const SessionStoreResult saved = save();
    if (saved.ok) mSessionId = saved.session_id;
  }
  return self;
}

}  // namespace agent
