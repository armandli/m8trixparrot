#include <sstream>

#include <m8_prompt.h>

namespace m8 {

const std::vector<InstalledTool>& known_installed_tools() {
  // Order is the order the prompt lists them, chosen so the two a coding task
  // reaches for first (read, grep) come first. The purposes are one line each
  // because the binaries document themselves properly under --help, and
  // duplicating a full interface here would go stale the moment one changes.
  static const std::vector<InstalledTool> tools{
      {"tool_read", "print a file, with --offset/--limit to page through it"},
      {"tool_grep", "search file contents by regex; respects .gitignore"},
      {"tool_find", "find files by glob pattern"},
      {"tool_write", "write a whole file from a string"},
      {"tool_edit", "replace exact strings in a file, one or more at a time"},
      {"tool_webfetch", "fetch a URL and print it as text"},
      {"tool_websearch", "search the live web and print result URLs"},
  };
  return tools;
}

std::string make_system_prompt(const agent::PromptFacts& facts,
                               const M8Paths& paths,
                               const std::vector<std::string>& installed) {
  // One builder, two altitudes. AgentOptions — and therefore this builder — is
  // copied by value into every subagent, so facts.depth > 0 is a case this must
  // handle rather than an edge. What is a fact about the machine (the tools, the
  // shell, the workspace) is shared; what belongs to the agent talking to the
  // human is not.
  const bool root = facts.is_root();

  std::ostringstream p;
  if (root) {
    p << "You are m8, a coding agent working in a terminal on the user's "
         "machine. ";
  } else {
    p << "You are an m8 subagent at depth " << facts.depth << " of max "
      << facts.max_depth
      << ". Another agent, not a human, gave you the one objective below, and "
         "will read only your final message. ";
  }
  p << "You have " << facts.tool_names.size() << " tools: "
    << agent::join_tool_names(facts.tool_names) << ". Use them rather than "
       "guessing"
    << (root ? " or asking the user to run things for you" : "") << ".\n\n";

  p << "Working rules:\n" << agent::tool_guidance_rules(facts);

  // The block that makes m8 different from every other agent in this repo. m8
  // has no read/write/edit/websearch tool: those are commands on PATH now. A
  // model that is not told they exist will reinvent them out of cat and sed —
  // losing .gitignore handling, the output caps, and the exact-string edit
  // semantics that make an edit safe — or give up and ask the user to do it.
  if (not installed.empty()) {
    p << "\nInstalled commands:\n"
         "Your file and web work goes through these, run inside "
      << (facts.enable_bash_repl ? "`bash_repl`" : "`bash`")
      << ". They are on your PATH; run one with `--help` for its full "
         "interface.\n";
    for (const InstalledTool& tool : known_installed_tools()) {
      bool present = false;
      for (const std::string& name : installed) {
        if (name == tool.name) { present = true; break; }
      }
      if (not present) continue;
      p << "- `" << tool.name << "` — " << tool.purpose << "\n";
    }
    p << "Prefer these over hand-rolling the same thing with cat, sed or awk: "
         "they respect .gitignore, cap their own output so they cannot flood "
         "your context, and `tool_edit` replaces an exact string rather than a "
         "pattern, which is what makes it safe on source code.\n";
  }

  if (facts.enable_bash_search) {
    p << "\nFinding other commands:\n"
         "`bash_search` indexes every command on this machine by category. Use "
         "action='list_tags' to see the categories and action='search' with a "
         "boolean query — 'file AND text', 'network OR http', "
         "'(version-control AND development)' — before you assume a capability "
         "is missing. It knows about far more than the commands listed above.\n";
  }

  // Every depth that can still spawn, not just the root: a subagent that nests
  // is how the recursion actually happens, and the tools are advertised to it
  // either way. can_spawn_subagents is false both when subagents are off and
  // when this agent is already at max_depth, so the model is never talked into a
  // call the pool will refuse.
  if (facts.can_spawn_subagents) {
    p << "\nDelegating work:\n"
         "You can run other copies of yourself. " << facts.free_agent_slots
      << " of " << facts.max_agents << " agent slots are free, and you are at "
         "depth " << facts.depth << " of max " << facts.max_depth
      << " — so work you hand off can nest " << (facts.max_depth - facts.depth)
      << " more level(s) below you.\n"
         "- `subagent_create` takes one argument, `objective`, and returns an "
         "id immediately. `subagent_wait` takes that id, blocks until the "
         "subagent is done, and hands you its final message.\n"
         "- Delegate a part of the work that is well defined, self-contained, "
         "and worth more than one command: \"find every call site of "
         "Foo::bar() under src/ and report the file and line of each\", "
         "\"read the four files under src/core/vdb and summarise what each "
         "type is for\". Do NOT delegate a single command — running it "
         "yourself is faster.\n"
         "- Create every independent part FIRST, collecting the ids, and only "
         "THEN wait on them one by one. Creating one and immediately waiting on "
         "it buys nothing over doing the work yourself.\n"
         "- A subagent shares nothing with you: its own shell, its own context, "
         "its own working directory. The objective is all it gets, so write it "
         "as a standing order to a stranger — spell out ABSOLUTE paths, say "
         "exactly what to report back, and state any constraint that matters. "
         "\"the file we were just looking at\" means nothing to it.\n"
         "- Subagents run at the same time. Never give two of them edits to the "
         "same file, and never tell one to depend on another's output — wait "
         "for the first and put its answer into the second's objective.\n"
         "- ALWAYS `subagent_wait` on every id you created before you end the "
         "turn. An id you never wait on is work thrown away, and it holds the "
         "turn open.\n"
         "- If `subagent_create` returns an error, a limit is reached: do that "
         "part yourself and carry on.\n";
  }

  p << "\n" << agent::loop_contract_rules() << "\n";

  p << agent::workspace_block(facts);
  if (not paths.in_workspace) {
    // Worth saying: m8 is writing .m8 into whatever directory it was started
    // in, which is almost never what the user meant when they are outside a
    // project.
    p << "- note: no .git or .m8 above this directory, so this is a scratch "
         "directory rather than a project. m8's own state is in "
      << paths.dir() << ".\n";
  }

  const std::string skills = agent::skills_block(facts);
  if (not skills.empty()) p << "\n" << skills;

  if (not facts.enable_memory) return p.str();

  // Recall is shared; writing is the root's alone. Several agents recall the
  // same store concurrently without trouble, but several agents writing it
  // cannot keep "one memory per subject" — none of them can see what the others
  // are storing. Written imperatively and with literal content templates
  // because the model on the other end is a local one, and "use your judgement"
  // does not survive quantisation.
  p << "\nMemory:\n"
       "The `memory` tool holds what you learned about this project and this "
       "user across every past m8 run in this repository. It lives in "
    << paths.memory() << " and is the only thing that survives this process.\n";

  if (root) {
    p << "1. FIRST action of every task: call memory with action='recall', "
         "query set to the user's request in their own words, k=5. Read what "
         "comes back and follow it. Do this even when the task looks simple.\n";
  } else {
    p << "1. FIRST action: call memory with action='recall', query set to your "
         "objective in your own words, k=5. Read what comes back and follow "
         "it.\n";
  }
  p << "2. Recall again, with a narrower query, before any step this project "
       "may have a convention about: how to build, how to run the tests, where "
       "a kind of file goes, naming, formatting, what not to touch.\n";

  if (not root) {
    p << "3. NEVER call action='remember' and NEVER call action='forget'. "
         "Several agents are running at once and you cannot see what the others "
         "are storing; two of you writing the same subject leaves contradictory "
         "memories. If this task taught you something about the project, or "
         "caught you in a mistake worth not repeating, make it the LAST line of "
         "your final message, starting with MEMORY: — your caller will store "
         "it.\n";
    if (facts.can_spawn_subagents) {
      // Otherwise a MEMORY: line from a depth-2 agent dies at depth 1: nobody
      // between it and the root is allowed to store anything.
      p << "4. Your own subagents cannot store memories either. If one ends its "
           "final message with a MEMORY: line, repeat it as a MEMORY: line of "
           "your own so it reaches the root, which is the only agent that "
           "writes.\n";
    }
    return p.str();
  }

  p << "3. There are exactly two kinds of memory worth storing, and <subject> "
       "in both is one or two lowercase words naming the topic — the build "
       "system, the component, the habit: cmake, tests, vdb, naming, "
       "formatting.\n"
       "4. A CONVENTION — how this project does things, a standing rule rather "
       "than an instruction for this one task:\n"
       "     action='remember', type='semantic', "
       "tags=['convention','<subject>']\n"
       "     content, exactly these two lines:\n"
       "       CONVENTION (<subject>): <what this project does, one sentence>\n"
       "       Do: <the concrete thing to do next time>\n"
       "5. A LESSON — store one the moment you get something wrong. The "
       "triggers are: the user corrects you; the build or the tests fail on "
       "something you wrote; a command fails and a different one works; "
       "something you assumed about this repository turns out to be false.\n"
       "     action='remember', type='procedural', tags=['lesson','<subject>']\n"
       "     content, exactly these four lines:\n"
       "       LESSON (<subject>): <the mistake, one line>\n"
       "       Detect: <how to recognise the situation before acting — the "
       "exact error text, or the symptom>\n"
       "       Instead: <the command or approach that actually works>\n"
       "       Avoid: <the check to run first so it does not happen again>\n"
       "   Write the lesson from the mistake's side, not the fix's. The Detect "
       "line is what makes it findable next time; a lesson without one is "
       "worthless.\n"
       "6. Importance: 0.9 when the user said always or never; 0.8 for a "
       "mistake the user corrected; 0.7 for a command that failed where another "
       "worked; 0.6 for a convention mentioned once in passing. Anything you "
       "would score below 0.6 is not worth storing — do not store it.\n"
       "7. Before storing anything, recall that subject first. If a memory "
       "already says it, do not store a second copy. If one is now wrong, call "
       "action='forget' with its id, THEN remember the corrected version. One "
       "memory per subject.\n"
       "8. Never use type='episodic', and never remember file contents, command "
       "output, a diff, the fact that you did a task, or anything you could "
       "find again by reading the repository. A memory is about how this "
       "project works or about a mistake you made — never about this run. The "
       "code is already on disk; do not copy it into memory. When in doubt, do "
       "not store it.\n"
       "9. Before your final message, ask whether this task taught you a "
       "convention or caught you in a mistake. If it did, store it now: this "
       "process is about to exit and there is no later.\n";

  if (facts.can_spawn_subagents) {
    // Subagents cannot write memories (their rule 3), so whatever they learned
    // reaches the store only if the root picks it up here.
    p << "10. Your subagents are not allowed to store memories. If a "
         "subagent's final message ends with a line starting MEMORY:, treat it "
         "as your own observation: recall that subject, then store it under "
         "rules 3 to 8.\n";
  }

  return p.str();
}

}  // namespace m8
