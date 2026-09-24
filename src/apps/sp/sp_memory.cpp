#include <cctype>
#include <cstdlib>

#include <sstream>
#include <string>
#include <vector>

#include <sp_memory.h>

namespace sp {

namespace {

std::string trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end and
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin and
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// A memory's content is several lines by design (see the LESSON template in
// the system prompt); a listing shows the first one so one memory is one row.
std::string first_line(const std::string& text, size_t max_chars) {
  std::string line = text.substr(0, text.find('\n'));
  if (line.size() > max_chars) line = line.substr(0, max_chars - 3) + "...";
  return line;
}

// Two decimals, without dragging <iomanip> in for one call.
std::string two_places(double value) {
  std::string text = std::to_string(value);
  const size_t dot = text.find('.');
  if (dot != std::string::npos and text.size() > dot + 3) {
    text = text.substr(0, dot + 3);
  }
  return text;
}

std::string human_bytes(uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
  return std::to_string(bytes / (1024 * 1024)) + " MB";
}

}  // namespace

std::string default_memory_path(const std::string& home) {
  return home + "/.local/share/sp/memory.m8db";
}

std::string make_extra_prompt(const std::string& username,
                              const std::string& home, bool memory_enabled) {
  const std::string scripts = home + "/.local/share/sp/scripts";
  const std::string trash   = home + "/.local/share/Trash/files";
  std::ostringstream p;
  p << "You are shell-parrot (sp), a natural-language shell assistant. "
       "Your job is to carry out the user's task using shell commands.\n\n"
       "Shell environment:\n"
       "- User: " << username << "\n"
       "- Home: " << home << "\n"
       "- Scripts directory: " << scripts
    << " (create it first if it does not exist; write any helper scripts here)\n"
       "- Trash: " << trash
    << " — NEVER delete files; move them with 'mv <path> " << trash
    << "/' instead\n\n"
       "Rules:\n"
       "1. NEVER use rm, rmdir, unlink, or any deletion command. "
          "Move to trash instead.\n"
       "2. Use bash_search before complex tasks if you are unsure which "
          "commands are available.\n"
       "3. Write any helper scripts to the scripts directory, not to the "
          "working directory.\n"
       "4. When done, give a clear English summary of what you did and "
          "whether it succeeded.\n"
       "5. If a task is risky or irreversible, state what you are about "
          "to do before acting.\n";

  if (not memory_enabled) return p.str();

  // Numbered from 6 so the memory rules read as a continuation of the five
  // above rather than a second, competing list. Written imperatively and with
  // a literal content template because the model on the other end is a local
  // one, and "use your judgement" does not survive quantisation.
  p << "\nMemory:\n"
       "The `memory` tool holds what you have learned about this user across "
       "every past sp run. It is the only thing that survives after this "
       "process exits.\n"
       " 6. FIRST action of every task: call memory with action='recall', "
          "query set to the user's request in their own words, k=5. Read what "
          "comes back and follow it. Do this even when the task looks "
          "simple.\n"
       " 7. Recall again, with a narrower query, before any step the user may "
          "have an opinion about: which command to use, where files go, "
          "naming, formatting, whether to ask before acting.\n"
       " 8. There are exactly two kinds of memory worth storing, and "
          "<subject> in both is one or two lowercase words naming the topic — "
          "the command, the file type, or the habit: git, tar, find, ssh, "
          "naming, permissions.\n"
       " 9. A PREFERENCE — how the user wants things done from now on, a "
          "standing rule and not an instruction for this one task:\n"
       "      action='remember', type='semantic', "
          "tags=['preference','<subject>']\n"
       "      content, exactly these two lines:\n"
       "        PREFERENCE (<subject>): <what the user wants, one sentence>\n"
       "        Do: <the concrete thing to do next time>\n"
       "10. A LESSON — store one the moment you get something wrong. The "
          "triggers are: the user corrects you; the user says no, don't, or "
          "not like that; the user makes you redo something; a command fails "
          "and a different one works; something you assumed about this "
          "machine turns out to be false.\n"
       "      action='remember', type='procedural', "
          "tags=['lesson','<subject>']\n"
       "      content, exactly these four lines:\n"
       "        LESSON (<subject>): <the mistake, one line>\n"
       "        Detect: <how to recognise the situation before acting — the "
          "exact error text, or the symptom>\n"
       "        Instead: <the command or approach that actually works>\n"
       "        Avoid: <the check to run first so it does not happen again>\n"
       "    Write the lesson from the mistake's side, not the fix's. The "
          "Detect line is what makes it findable next time; a lesson without "
          "one is worthless.\n"
       "11. Importance: 0.9 when the user said always or never; 0.8 for a "
          "mistake the user corrected; 0.7 for a command that failed where "
          "another worked; 0.6 for a preference mentioned once in passing. "
          "Anything you would score below 0.6 is not worth storing — do not "
          "store it.\n"
       "12. Before storing anything, recall that subject first. If a memory "
          "already says it, do not store a second copy. If one is now wrong, "
          "call action='forget' with its id, THEN remember the corrected "
          "version. One memory per subject.\n"
       "13. Never use type='episodic', and never remember command output, "
          "directory listings, file contents, the fact that you ran a task, "
          "anything true only of the directory you happen to be in, or "
          "anything you could find again by running a command. A memory is "
          "about the user or about you — never about this run. When in doubt, "
          "do not store it.\n"
       "14. Before your closing summary in rule 4, ask whether this task "
          "taught you a preference or caught you in a mistake. If it did, "
          "store it now: this process is about to exit and there is no "
          "later.\n";
  return p.str();
}

Command parse_command(const std::string& input) {
  Command command;
  const std::string text = trim(input);
  if (text.empty() or text[0] != '/') return command;

  const size_t space = text.find_first_of(" \t");
  const std::string name = text.substr(0, space);
  const std::string args =
      space == std::string::npos ? std::string() : trim(text.substr(space + 1));

  if (name == "/quit") {
    command.kind = Command::Kind::Quit;
  } else if (name == "/help") {
    command.kind = Command::Kind::Help;
  } else if (name == "/reset") {
    command.kind = Command::Kind::Reset;
  } else if (name == "/remember") {
    command.kind = Command::Kind::Remember;
    command.args = args;
  } else if (name == "/memories") {
    command.kind = Command::Kind::Memories;
    command.args = args;
  } else if (name == "/forget") {
    // strtoull would accept "12abc" and a negative id wraps around, so the
    // digits are checked before the conversion rather than after it.
    const bool digits =
        not args.empty() and
        args.find_first_not_of("0123456789") == std::string::npos;
    if (digits) {
      command.kind = Command::Kind::Forget;
      command.id = std::strtoull(args.c_str(), nullptr, 10);
      if (command.id == 0) command.kind = Command::Kind::BadForget;
    } else {
      command.kind = Command::Kind::BadForget;
    }
  }
  // An unrecognised /word stays Kind::None and goes to the agent as a task,
  // which is what sp did before any of these commands existed.
  return command;
}

std::string do_remember(agent::MemoryStore& store, const std::string& text) {
  if (text.empty()) {
    return "usage: /remember <something about you or how you like things "
           "done>";
  }
  agent::Memory memory;
  memory.content = text;
  // Semantic and tagged as a preference: what a person types by hand is a
  // standing fact about them, not something that happened. The context_id is
  // left at the tool's own default so the agent's recalls see these too.
  memory.memory_type = agent::kMemorySemantic;
  memory.importance = 0.8;
  memory.tags = {"preference"};

  const agent::RememberResult stored = store.remember(memory);
  if (not stored.ok) return stored.error;
  return "remembered as memory " + std::to_string(stored.id);
}

std::string do_search(agent::MemoryStore& store, const std::string& query) {
  if (query.empty()) {
    const agent::MemoryStats stats = store.stats();
    std::ostringstream out;
    out << stats.total << " memories";
    if (stats.total > 0) {
      out << " (" << stats.semantic << " preference/fact, " << stats.procedural
          << " lesson, " << stats.episodic << " episodic";
      if (stats.other > 0) out << ", " << stats.other << " other";
      out << ")";
    }
    if (stats.dim > 0) out << ", " << stats.dim << "-dim";
    if (stats.file_bytes > 0) out << ", " << human_bytes(stats.file_bytes);
    out << "\n" << stats.path;
    if (stats.total == 0) out << "\n(nothing remembered yet)";
    return out.str();
  }

  agent::RecallQuery recall;
  recall.query = query;
  recall.k = 10;

  const agent::RecallResult recalled = store.recall(recall);
  if (not recalled.ok) return recalled.error;
  if (recalled.memories.empty()) return "no memories matched '" + query + "'";

  std::ostringstream out;
  for (const agent::ScoredMemory& scored : recalled.memories) {
    out << "[" << scored.memory.id << "] " << scored.memory.memory_type << " ("
        << two_places(scored.score) << ") "
        << first_line(scored.memory.content, 100) << "\n";
  }
  out << "(/forget <id> to delete one)";
  return out.str();
}

std::string do_forget(agent::MemoryStore& store, uint64_t id) {
  const agent::StoreResult forgotten = store.forget({id});
  if (not forgotten.ok) return forgotten.error;
  return "forgot memory " + std::to_string(id);
}

}  // namespace sp
