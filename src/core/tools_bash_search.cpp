#include <core/tools.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <simdjson.h>

#include <core/util/json_util.h>
#include <core/tools_util.h>

namespace agent {

namespace {

// Set once at startup, read by BashSearchIndex::index_path(). A function-local
// static rather than a namespace-scope one so there is no initialisation-order
// question with the index singleton that reads it.
std::string& index_path_override() {
  static std::string path;
  return path;
}

// Whether the index singleton currently exists. Constant-initialised, so it is
// readable before it is first constructed and after it is destroyed — which
// lets the free functions below wait on a rescan without *constructing* the
// singleton merely to discover there is nothing to wait for.
std::atomic<bool> g_index_live{false};

// ─────────────────────────── data model ──────────────────────────────────────

struct CommandEntry {
  std::string name;
  std::string description;
  std::string usage;
  std::vector<std::string> tags;
};

// ───────────────────────── tag knowledge base ─────────────────────────────────

// Direct command-name → tags for well-known tools.
const std::map<std::string, std::vector<std::string>, std::less<>> kNameTags{
    {"ls",           {"file", "listing"}},
    {"cp",           {"file"}},
    {"mv",           {"file"}},
    {"rm",           {"file"}},
    {"mkdir",        {"file"}},
    {"rmdir",        {"file"}},
    {"chmod",        {"file", "user"}},
    {"chown",        {"file", "user"}},
    {"ln",           {"file"}},
    {"touch",        {"file"}},
    {"stat",         {"file", "system"}},
    {"find",         {"file", "search"}},
    {"du",           {"file", "system"}},
    {"df",           {"file", "system"}},
    {"lsblk",        {"system"}},
    {"mount",        {"system"}},
    {"umount",       {"system"}},
    {"sync",         {"system"}},
    {"dmesg",        {"system", "monitoring"}},
    {"uname",        {"system"}},
    {"free",         {"system", "monitoring"}},
    {"uptime",       {"system", "monitoring"}},
    {"hostname",     {"network", "system"}},
    {"grep",         {"text", "search"}},
    {"sed",          {"text"}},
    {"awk",          {"text"}},
    {"cut",          {"text"}},
    {"sort",         {"text"}},
    {"uniq",         {"text"}},
    {"tr",           {"text"}},
    {"wc",           {"text"}},
    {"head",         {"text", "file"}},
    {"tail",         {"text", "file", "monitoring"}},
    {"cat",          {"text", "file"}},
    {"tee",          {"text"}},
    {"diff",         {"text"}},
    {"patch",        {"text"}},
    {"column",       {"text"}},
    {"curl",         {"network"}},
    {"wget",         {"network"}},
    {"ssh",          {"network", "security"}},
    {"scp",          {"network", "file"}},
    {"rsync",        {"file", "network"}},
    {"ping",         {"network"}},
    {"traceroute",   {"network"}},
    {"netstat",      {"network", "monitoring"}},
    {"ss",           {"network", "monitoring"}},
    {"ip",           {"network"}},
    {"ifconfig",     {"network"}},
    {"nslookup",     {"network"}},
    {"dig",          {"network"}},
    {"host",         {"network"}},
    {"nc",           {"network"}},
    {"ncat",         {"network"}},
    {"nmap",         {"network", "security"}},
    {"tcpdump",      {"network", "monitoring"}},
    {"ps",           {"process"}},
    {"kill",         {"process"}},
    {"killall",      {"process"}},
    {"pkill",        {"process"}},
    {"top",          {"process", "monitoring"}},
    {"htop",         {"process", "monitoring"}},
    {"nice",         {"process"}},
    {"renice",       {"process"}},
    {"nohup",        {"process"}},
    {"pgrep",        {"process", "search"}},
    {"jobs",         {"process", "shell"}},
    {"lsof",         {"file", "network", "process"}},
    {"strace",       {"process", "monitoring"}},
    {"ltrace",       {"process", "monitoring"}},
    {"tar",          {"archive"}},
    {"gzip",         {"archive"}},
    {"gunzip",       {"archive"}},
    {"zip",          {"archive"}},
    {"unzip",        {"archive"}},
    {"bzip2",        {"archive"}},
    {"bunzip2",      {"archive"}},
    {"xz",           {"archive"}},
    {"7z",           {"archive"}},
    {"zstd",         {"archive"}},
    {"git",          {"development", "version-control"}},
    {"svn",          {"development", "version-control"}},
    {"hg",           {"development", "version-control"}},
    {"gcc",          {"development"}},
    {"g++",          {"development"}},
    {"clang",        {"development"}},
    {"clang++",      {"development"}},
    {"make",         {"development"}},
    {"cmake",        {"development"}},
    {"ninja",        {"development"}},
    {"gdb",          {"development"}},
    {"lldb",         {"development"}},
    {"valgrind",     {"development"}},
    {"nm",           {"development"}},
    {"objdump",      {"development"}},
    {"ldd",          {"development"}},
    {"docker",       {"container"}},
    {"podman",       {"container"}},
    {"kubectl",      {"container"}},
    {"helm",         {"container"}},
    {"sqlite3",      {"database"}},
    {"mysql",        {"database", "network"}},
    {"psql",         {"database", "network"}},
    {"redis-cli",    {"database", "network"}},
    {"mongo",        {"database", "network"}},
    {"vim",          {"editor"}},
    {"vi",           {"editor"}},
    {"nano",         {"editor"}},
    {"emacs",        {"editor"}},
    {"nvim",         {"editor"}},
    {"bash",         {"shell"}},
    {"sh",           {"shell"}},
    {"zsh",          {"shell"}},
    {"fish",         {"shell"}},
    {"echo",         {"shell", "text"}},
    {"printf",       {"shell", "text"}},
    {"env",          {"shell", "system"}},
    {"xargs",        {"shell", "text"}},
    {"which",        {"shell", "search"}},
    {"whereis",      {"shell", "search"}},
    {"man",          {"shell", "text"}},
    {"whatis",       {"search", "shell"}},
    {"apropos",      {"search", "shell"}},
    {"gpg",          {"security"}},
    {"openssl",      {"network", "security"}},
    {"ssh-keygen",   {"network", "security"}},
    {"sudo",         {"security", "user"}},
    {"su",           {"user"}},
    {"passwd",       {"security", "user"}},
    {"useradd",      {"user"}},
    {"userdel",      {"user"}},
    {"usermod",      {"user"}},
    {"groupadd",     {"user"}},
    {"id",           {"user"}},
    {"who",          {"user"}},
    {"last",         {"user"}},
    {"apt",          {"package"}},
    {"apt-get",      {"package"}},
    {"dpkg",         {"package"}},
    {"snap",         {"package"}},
    {"pip",          {"development", "package"}},
    {"pip3",         {"development", "package"}},
    {"npm",          {"development", "package"}},
    {"yarn",         {"development", "package"}},
    {"cargo",        {"development", "package"}},
    {"brew",         {"package"}},
    {"pacman",       {"package"}},
    {"yum",          {"package"}},
    {"dnf",          {"package"}},
    {"ffmpeg",       {"media"}},
    {"convert",      {"media"}},
    {"exiftool",     {"media"}},
    {"watch",        {"monitoring"}},
    {"vmstat",       {"monitoring", "system"}},
    {"iostat",       {"monitoring", "system"}},
    {"sar",          {"monitoring", "system"}},
    {"perf",         {"development", "monitoring"}},
    {"crontab",      {"scheduling"}},
    {"at",           {"scheduling"}},
    {"systemctl",    {"scheduling", "system"}},
    {"service",      {"scheduling", "system"}},
    {"jq",           {"serialization", "text"}},
    {"yq",           {"serialization", "text"}},
    {"xmllint",      {"serialization", "text"}},
    {"base64",       {"serialization", "text"}},
    {"xxd",          {"serialization", "text"}},
    {"bc",           {"math"}},
    {"expr",         {"math", "shell"}},
    {"python",       {"development", "math", "shell"}},
    {"python3",      {"development", "math", "shell"}},
    // shell-parrot (sp): natural-language shell assistant.
    {"sp",           {"shell", "development"}},
    {"shell-parrot", {"shell", "development"}},
    // m8trixparrot file tools exposed as standalone CLI utilities.
    {"tool_find",    {"file", "search"}},
    {"tool_grep",    {"file", "text", "search", "filter"}},
    {"tool_read",    {"file", "text"}},
    {"tool_webfetch",  {"network", "text"}},
    {"tool_websearch", {"network", "search"}},
    {"tool_write",   {"file"}},
    {"tool_edit",    {"file", "text"}},
};

// Keywords in a whatis description that imply certain tags.
const std::vector<std::pair<std::string, std::vector<std::string>>> kKeywordTags{
    {"version control",   {"development", "version-control"}},
    {"source control",    {"development", "version-control"}},
    {"package manager",   {"package"}},
    {"text editor",       {"editor"}},
    {"container",         {"container"}},
    {"kubernetes",        {"container"}},
    {"database",          {"database"}},
    {"compress",          {"archive"}},
    {"archive",           {"archive"}},
    {"network",           {"network"}},
    {"socket",            {"network"}},
    {"encrypt",           {"security"}},
    {"decrypt",           {"security"}},
    {"certificate",       {"security"}},
    {"authentication",    {"security"}},
    {"monitor",           {"monitoring"}},
    {"schedule",          {"scheduling"}},
    {"directory",         {"file"}},
    {"file",              {"file"}},
    {"process",           {"process"}},
    {"daemon",            {"process"}},
    {"signal",            {"process"}},
    {"search",            {"search"}},
    {"pattern",           {"search", "text"}},
    {"text",              {"text"}},
    {"string",            {"text"}},
    {"user",              {"user"}},
    {"group",             {"user"}},
    {"permission",        {"file", "user"}},
    {"json",              {"serialization", "text"}},
    {"xml",               {"serialization", "text"}},
    {"yaml",              {"serialization", "text"}},
    {"serial",            {"serialization"}},
    {"math",              {"math"}},
    {"numeric",           {"math"}},
    {"arithmetic",        {"math"}},
    {"image",             {"media"}},
    {"audio",             {"media"}},
    {"video",             {"media"}},
    {"shell",             {"shell"}},
    {"system",            {"system"}},
    {"memory",            {"monitoring", "system"}},
    {"disk",              {"file", "system"}},
    {"compile",           {"development"}},
    {"debug",             {"development"}},
    {"build",             {"development"}},
};

// ──────────────────── manual page description source ─────────────────────────

// One parsed line of an apropos(1)/whatis(1) dump.
struct ManPageLine {
  std::vector<std::string> names;  // Empty when the line is not in that form.
  int section = 0;                 // 0 when the line names no section.
  std::string description;
};

// Splits a line such as
//   ls(1)                     - list directory contents
//   grep(1), egrep(1)         - file pattern searcher
//   bzegrep, bzfgrep (1)      -- search compressed files for a pattern
// into its names, section and description. Both spellings of the name list are
// accepted — mandoc's "name(1)" and man-db's "name (1)" — as is a list that
// carries the section only once, after its last name.
ManPageLine parse_man_page_line(std::string_view line) {
  ManPageLine parsed;

  // The separator is whichever of " - " / " -- " comes first: a description
  // may itself contain a "--", so searching for the long form across the whole
  // line would cut in the wrong place.
  const auto dash  = line.find(" - ");
  const auto ddash = line.find(" -- ");
  size_t sep = dash;
  size_t sep_width = 3;
  if (ddash < sep) {
    sep = ddash;
    sep_width = 4;
  }
  if (sep == std::string_view::npos) return parsed;

  parsed.description = std::string(line.substr(sep + sep_width));

  std::string_view names = line.substr(0, sep);
  while (!names.empty()) {
    const auto comma = names.find(',');
    std::string_view piece = names.substr(0, comma);
    names = comma == std::string_view::npos ? std::string_view()
                                            : names.substr(comma + 1);

    // Trim, then peel a trailing "(section)" off the name it belongs to.
    while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front())))
      piece.remove_prefix(1);
    while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back())))
      piece.remove_suffix(1);
    if (!piece.empty() && piece.back() == ')') {
      const auto open = piece.rfind('(');
      if (open != std::string_view::npos) {
        if (parsed.section == 0) {
          const std::string_view sec = piece.substr(open + 1, piece.size() - open - 2);
          if (!sec.empty() && std::isdigit(static_cast<unsigned char>(sec.front())))
            parsed.section = sec.front() - '0';
        }
        piece = piece.substr(0, open);
        while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back())))
          piece.remove_suffix(1);
      }
    }
    if (!piece.empty()) parsed.names.emplace_back(piece);
  }

  return parsed;
}

// How much a manual section is worth as the description of a *command*: user
// commands first, then admin commands and games, with library calls and file
// formats last. Lower is better.
int section_rank(int section) {
  switch (section) {
    case 1:  return 0;
    case 8:  return 1;
    case 6:  return 2;
    case 0:  return 3;
    default: return 4 + section;
  }
}

// Every manual page description the system knows, keyed by command name.
//
// The obvious spelling — one whatis(1) query per command name — is what made a
// scan unusable on macOS: mandoc has no prebuilt index there, so each query
// re-reads the whole manual tree (~0.9s), and a 2000-entry PATH turned that
// into half an hour of scanning. Both man-db and mandoc will instead dump
// every entry they know in a single call, which costs about a second on either
// platform, so the scan asks once and indexes the answer itself.
//
// Returns an empty map when the system has no usable manual index at all; that
// is not an error, it just leaves descriptions blank and tagging to kNameTags.
std::map<std::string, std::string, std::less<>> load_man_descriptions() {
  // A bound, not a budget: even the fallbacks below finish in about a second,
  // so this only exists to keep a pathological `man` from wedging the scan.
  constexpr int kDumpTimeoutSeconds = 30;

  // apropos(1) treats its argument as a regular expression by default on both
  // man-db and mandoc, so "." selects every page. `man -k` is the same search
  // under another name, kept for systems that install only man(1).
  static const char* const kDumpCommands[] = {
      "apropos . 2>/dev/null",
      "man -k . 2>/dev/null",
  };

  std::map<std::string, std::string, std::less<>> descriptions;
  std::map<std::string, int, std::less<>> best_rank;

  for (const char* command : kDumpCommands) {
    const std::string out = run_shell_capture(command, kDumpTimeoutSeconds);
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
      const ManPageLine parsed = parse_man_page_line(line);
      if (parsed.names.empty() || parsed.description.empty()) continue;
      const int rank = section_rank(parsed.section);
      for (const auto& name : parsed.names) {
        const auto it = best_rank.find(name);
        if (it != best_rank.end() && it->second <= rank) continue;
        best_rank[name]    = rank;
        descriptions[name] = parsed.description;
      }
    }
    if (!descriptions.empty()) break;
  }

  return descriptions;
}

// ─────────────────────────── tag assignment ───────────────────────────────────

std::vector<std::string> assign_tags(const std::string& name,
                                     const std::string& description) {
  std::set<std::string> result;

  // Direct name lookup (case-sensitive; command names are case-sensitive).
  const auto it = kNameTags.find(name);
  if (it != kNameTags.end()) {
    for (const auto& t : it->second) result.insert(t);
  }

  // Keyword matching on lowercased description.
  std::string lower = description;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  for (const auto& [kw, tags] : kKeywordTags) {
    if (lower.find(kw) != std::string::npos) {
      for (const auto& t : tags) result.insert(t);
    }
  }

  return {result.begin(), result.end()};
}

// ─────────────────────── boolean tag query parser ─────────────────────────────
//
// Grammar (AND binds tighter than OR):
//   query    := or_expr
//   or_expr  := and_expr ('OR' and_expr)*
//   and_expr := atom ('AND' atom)*
//   atom     := TAG | '(' query ')'
//
// Tokens are whitespace-separated words; '(' and ')' are split off word edges.
// Parsing yields a tree that is then evaluated once per index entry — the
// parser used to run again for every entry, which made each search re-tokenize
// the query a couple of thousand times for no gain.

struct TagExpr {
  enum class Kind { Tag, And, Or };

  Kind kind = Kind::Tag;
  std::string tag;                // Kind::Tag only.
  std::vector<TagExpr> children;  // Kind::And / Kind::Or only.

  // `tags` must be sorted, which is how CommandEntry stores them.
  bool eval(const std::vector<std::string>& tags) const {
    switch (kind) {
      case Kind::Tag:
        return std::binary_search(tags.begin(), tags.end(), tag);
      case Kind::And:
        return std::all_of(children.begin(), children.end(),
                           [&](const TagExpr& c) { return c.eval(tags); });
      case Kind::Or:
        return std::any_of(children.begin(), children.end(),
                           [&](const TagExpr& c) { return c.eval(tags); });
    }
    return false;
  }
};

struct TagQueryParser {
  std::vector<std::string> tokens;
  size_t pos = 0;
  std::string error;

  explicit TagQueryParser(const std::string& query) {
    std::istringstream ss(query);
    std::string tok;
    while (ss >> tok) {
      // Peel leading/trailing parentheses into their own tokens.
      for (size_t i = 0; i < tok.size();) {
        if (tok[i] == '(' || tok[i] == ')') {
          tokens.push_back(tok.substr(i, 1));
          ++i;
        } else {
          size_t j = i;
          while (j < tok.size() && tok[j] != '(' && tok[j] != ')') ++j;
          if (j > i) tokens.push_back(tok.substr(i, j - i));
          i = j;
        }
      }
    }
  }

  std::optional<TagExpr> parse() {
    auto expr = parse_or();
    if (!expr) return {};
    if (!at_end()) { error = "unexpected token '" + peek() + "'"; return {}; }
    return expr;
  }

private:
  bool at_end() const { return pos >= tokens.size(); }
  const std::string& peek() const { return tokens[pos]; }
  std::string consume() { return tokens[pos++]; }

  // Folds a run of same-precedence operands into one n-ary node, so
  // "a OR b OR c" is a single Or over three children rather than a chain.
  std::optional<TagExpr> parse_run(TagExpr::Kind kind, const std::string& op,
                                   std::optional<TagExpr> (TagQueryParser::*next)()) {
    auto left = (this->*next)();
    if (!left) return {};
    if (at_end() || peek() != op) return left;

    TagExpr node;
    node.kind = kind;
    node.children.push_back(std::move(*left));
    while (!at_end() && peek() == op) {
      consume();
      auto right = (this->*next)();
      if (!right) return {};
      node.children.push_back(std::move(*right));
    }
    return node;
  }

  std::optional<TagExpr> parse_or() {
    return parse_run(TagExpr::Kind::Or, "OR", &TagQueryParser::parse_and);
  }

  std::optional<TagExpr> parse_and() {
    return parse_run(TagExpr::Kind::And, "AND", &TagQueryParser::parse_atom);
  }

  std::optional<TagExpr> parse_atom() {
    if (at_end()) { error = "unexpected end of query"; return {}; }
    if (peek() == "(") {
      consume();
      auto inner = parse_or();
      if (!inner) return {};
      if (at_end() || peek() != ")") { error = "missing closing ')'"; return {}; }
      consume();
      return inner;
    }
    if (peek() == ")") { error = "unexpected ')'"; return {}; }
    TagExpr node;
    node.kind = TagExpr::Kind::Tag;
    node.tag  = consume();
    return node;
  }
};

// Parses `query` into an evaluable tree. On a malformed query returns nullopt
// and sets `error` to a message naming what went wrong.
std::optional<TagExpr> parse_tag_query(const std::string& query,
                                       std::string& error) {
  TagQueryParser parser(query);
  auto expr = parser.parse();
  if (!expr) {
    error = parser.error.empty() ? "malformed query" : parser.error;
  }
  return expr;
}

// ─────────────────────────── index singleton ──────────────────────────────────

class BashSearchIndex {
public:
  using Index = std::map<std::string, CommandEntry>;

  static BashSearchIndex& instance() {
    static BashSearchIndex inst;
    return inst;
  }

  // Signals the worker to give up on anything it has not started, then joins.
  // Joining rather than detaching is what makes a background rescan legal at
  // all: this is a function-local static, so a detached thread would race its
  // own destruction — and would write through index_path() after a test has
  // reset the override, clobbering the developer's real index file.
  ~BashSearchIndex() {
    mStop.store(true);
    wait_for_rescan();
    g_index_live.store(false);
  }

  // Serves the cached index, then refreshes it in the background.
  //
  // The cache is what makes this fast: a warm load is a few milliseconds
  // against the ~1.4s the scan costs, essentially all of it one `apropos`
  // subprocess. The old code ran that scan inline, under the lock, whenever
  // the file was missing. Now it runs on the worker and only a cold start —
  // no usable cache to serve — still blocks.
  void ensure_loaded() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (not mLoaded) {
      if (load_locked()) {
        // Served from cache. It may be stale — a command installed since the
        // last scan is missing from it — so refresh it behind the caller.
        start_rescan_locked();
      } else {
        // Nothing usable on disk, so there is nothing to serve and no way to
        // avoid paying for the scan once.
        mIndex = build_index(current_path_env());
        write_index(mIndex, index_path());
      }
      mLoaded = true;
    }
  }

  // Rebuilds the index from PATH + whatis, saves it, and returns a summary.
  // Deliberately synchronous: `action='scan'` means "rebuild it now", and its
  // result reports how many commands were found.
  std::string scan() {
    // Built off-lock, like the worker does, so a scan requested while a search
    // is running does not block that search for its whole duration.
    Index fresh = build_index(current_path_env());
    const std::string path = index_path();
    write_index(fresh, path);

    std::lock_guard<std::mutex> lock(mMutex);
    mIndex.swap(fresh);
    mLoaded = true;
    return "Scanned " + std::to_string(mIndex.size()) + " commands.";
  }

  // Returns sorted list of all tags present in the current index.
  std::vector<std::string> all_tags() const {
    std::lock_guard<std::mutex> lock(mMutex);
    std::set<std::string> tags;
    for (const auto& [_, e] : mIndex) {
      for (const auto& t : e.tags) tags.insert(t);
    }
    return {tags.begin(), tags.end()};
  }

  // Evaluates a boolean tag query against the index.
  // On parse error, returns empty and sets `error`.
  std::vector<CommandEntry> search(const std::string& query,
                                   std::string& error) const {
    const std::optional<TagExpr> expr = parse_tag_query(query, error);
    if (!expr) return {};

    std::lock_guard<std::mutex> lock(mMutex);
    std::vector<CommandEntry> results;
    for (const auto& [_, entry] : mIndex) {
      if (expr->eval(entry.tags)) results.push_back(entry);
    }
    return results;
  }

  // Drops what is loaded, so the next ensure_loaded() goes back through the
  // cache-load path. Test-only; see tools.h.
  void reset_for_test() {
    wait_for_rescan();
    std::lock_guard<std::mutex> lock(mMutex);
    mIndex.clear();
    mLoaded = false;
  }

  // Blocks until an in-flight rescan has finished. Tests need it to stay
  // deterministic, and anything about to change the index path needs it so the
  // worker cannot write through the old one.
  void wait_for_rescan() {
    std::thread worker;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      worker = std::move(mRescan);
    }
    if (worker.joinable()) worker.join();
  }

private:
  BashSearchIndex() { g_index_live.store(true); }

  mutable std::mutex mMutex;
  Index mIndex;
  bool mLoaded = false;
  std::thread mRescan;
  std::atomic<bool> mStop{false};

  std::string index_path() const {
    if (const std::string& override_path = index_path_override();
        not override_path.empty()) {
      return override_path;
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.m8trix/bash_search_index.json";
  }

  // Snapshotted on the calling thread rather than read inside build_index:
  // tools_python sets PIP_NO_INDEX and friends process-wide, and getenv
  // alongside another thread's setenv is not safe.
  static std::string current_path_env() {
    const char* path_env = std::getenv("PATH");
    return path_env != nullptr ? path_env : "";
  }

  // ── the worker ──

  // Must be called under mMutex. Starts one rescan; a second call while one is
  // already running is a no-op.
  void start_rescan_locked() {
    if (mRescan.joinable()) return;

    // Both the path and the environment are read here, on the caller's thread,
    // so the worker touches no shared mutable state but mIndex.
    const std::string path_env = current_path_env();
    const std::string path = index_path();

    mRescan = std::thread([this, path_env, path] {
      // Nothing expensive has started yet, so an early exit costs nothing.
      if (mStop.load()) return;

      // Built and written with no lock held. This is the whole point: the old
      // scan held mMutex for its entire ~1.4s, which would block every search
      // and list_tags call for that window.
      Index fresh = build_index(path_env);
      if (mStop.load() or fresh.empty()) return;
      write_index(fresh, path);

      // The only locked step, and it is a pointer swap. Safe because no
      // reference into mIndex ever escapes the lock — search() pushes copies
      // and all_tags() returns a fresh vector — so a reader can never hold
      // something this invalidates.
      std::lock_guard<std::mutex> lock(mMutex);
      mIndex.swap(fresh);
    });
  }

  // ── must be called under mMutex ──

  // False when the cache is missing OR unparseable. Telling those apart from
  // success is what stops a torn or truncated file from meaning "empty index,
  // forever": the caller treats false as cold and rebuilds.
  bool load_locked() {
    const auto text = read_file(index_path());
    if (!text) return false;

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(*text);
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return false;
    simdjson::ondemand::object root;
    if (doc.get_object().get(root)) return false;

    simdjson::ondemand::array arr;
    if (root["commands"].get_array().get(arr)) return false;

    Index loaded;
    for (auto item : arr) {
      simdjson::ondemand::object obj;
      if (item.get_object().get(obj)) continue;
      CommandEntry e;
      e.name        = util::string_field(obj, "name");
      e.description = util::string_field(obj, "description");
      e.usage       = util::string_field(obj, "usage");
      e.tags        = util::string_array_field(obj, "tags");
      // TagExpr::eval binary-searches the tags; assign_tags emits them sorted,
      // but an index file edited by hand need not be.
      std::sort(e.tags.begin(), e.tags.end());
      if (!e.name.empty()) loaded[e.name] = std::move(e);
    }
    // A file that parses but holds nothing is as useless as a missing one, and
    // is what a half-written file usually looks like.
    if (loaded.empty()) return false;

    mIndex = std::move(loaded);
    return true;
  }

  // ── lock-free: these touch no member ──

  // Written to a temp file and renamed, so a reader — this process next time,
  // or a second sp — sees either the old index or the new one, never half of
  // one. With a background writer that is no longer a theoretical concern.
  static void write_index(const Index& index, const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    if (ec) return;

    util::JsonWriter w;
    w.begin_object().key("commands").begin_array();
    for (const auto& [_, e] : index) {
      w.begin_object()
          .field("name", e.name)
          .field("description", e.description)
          .field("usage", e.usage)
          .field("tags", e.tags)
          .end_object();
    }
    w.end_array().end_object();

    const std::string temp = path + ".tmp";
    {
      std::ofstream out(temp);
      if (not out) return;
      out << w.str();
      if (not out) {
        std::filesystem::remove(temp, ec);
        return;
      }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) std::filesystem::remove(temp, ec);
  }

  // Builds a fresh index from `path_env`. Touches no member, so it runs on the
  // worker thread with nothing locked.
  static Index build_index(const std::string& path_env) {
    Index index;

    // 1. Enumerate executables in PATH, deduplicating by name.
    if (path_env.empty()) return index;

    std::set<std::string> seen;
    std::vector<std::string> names;
    std::istringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
      if (dir.empty()) continue;
      // A missing or unreadable PATH entry leaves the iterator at end(), so
      // the loop simply contributes nothing.
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        // One status() for both questions: asking is_regular_file separately
        // doubles the stat calls over a PATH with thousands of entries.
        std::error_code entry_ec;
        const auto status = e.status(entry_ec);  // Follows symlinks, as exec does.
        if (entry_ec || !std::filesystem::is_regular_file(status)) continue;
        constexpr auto kAnyExec = std::filesystem::perms::owner_exec |
                                  std::filesystem::perms::group_exec |
                                  std::filesystem::perms::others_exec;
        if ((status.permissions() & kAnyExec) == std::filesystem::perms::none)
          continue;
        const std::string name = e.path().filename().string();
        if (seen.insert(name).second) names.push_back(name);
      }
    }

    // 2. Pull every manual page description the system knows, in one command.
    //    Cost here is independent of how many executables PATH holds, and it
    //    is where essentially all of the scan's time goes.
    const auto descriptions =
        names.empty() ? std::map<std::string, std::string, std::less<>>()
                      : load_man_descriptions();

    // 3. Build index entries.
    for (const auto& name : names) {
      const auto desc = descriptions.find(name);
      CommandEntry entry;
      entry.name        = name;
      entry.description = desc == descriptions.end() ? std::string() : desc->second;
      entry.usage       = "";
      entry.tags        = assign_tags(name, entry.description);
      index[name]       = std::move(entry);
    }
    return index;
  }
};

// ──────────────────────── output formatting ───────────────────────────────────

std::string format_entry(const CommandEntry& e) {
  std::string out = e.name;
  if (!e.description.empty()) out += ": " + e.description;
  if (!e.usage.empty())       out += "\n  usage: " + e.usage;
  if (!e.tags.empty()) {
    out += " [";
    for (size_t i = 0; i < e.tags.size(); ++i) {
      if (i) out += ", ";
      out += e.tags[i];
    }
    out += "]";
  }
  return out;
}

}  // namespace

// ─────────────────────────── BashSearchTool ───────────────────────────────────

std::string BashSearchTool::description() const {
  return R"json({"name":"bash_search","description":"Find shell commands available on this system by category tag. Use action='list_tags' to see all tag categories, action='search' with a query like 'file AND text' or 'network OR security' to find relevant commands, or action='scan' to rebuild the command index from PATH.","parameters":{"type":"object","properties":{"action":{"type":"string","description":"list_tags: list all available tag categories; search: find commands by boolean tag query (AND/OR); scan: rebuild the command index from PATH"},"query":{"type":"string","description":"For action='search': boolean tag expression, e.g. 'file AND text', 'network OR security', '(file AND edit) OR editor'"}},"required":["action"]}})json";
}

ToolResult BashSearchTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const auto action = string_arg(args, "action");
  if (!action || action->empty()) {
    result.error = "bash_search: missing required string argument 'action'";
    return result;
  }

  auto& index = BashSearchIndex::instance();

  if (*action == "scan") {
    result.ok     = true;
    result.output = index.scan();
    return result;
  }

  index.ensure_loaded();

  if (*action == "list_tags") {
    const std::vector<std::string> tags = index.all_tags();
    if (tags.empty()) {
      result.ok     = true;
      result.output = "No tags found. Use action='scan' to build the command index.";
      return result;
    }
    std::string out;
    for (const auto& t : tags) out += t + "\n";
    result.ok     = true;
    result.output = std::move(out);
    return result;
  }

  if (*action == "search") {
    const auto query = string_arg(args, "query");
    if (!query || query->empty()) {
      result.error = "bash_search: action='search' requires a 'query' argument";
      return result;
    }
    std::string err;
    const std::vector<CommandEntry> matches = index.search(*query, err);
    if (!err.empty()) {
      result.error = "bash_search: query parse error: " + err;
      return result;
    }
    if (matches.empty()) {
      result.ok     = true;
      result.output = "No commands found matching: " + *query;
      return result;
    }
    std::string out;
    for (const auto& e : matches) out += format_entry(e) + "\n";
    TruncatedOutput trunc = truncate_output(std::move(out), "bash_search");
    result.ok           = true;
    result.output       = std::move(trunc.text);
    result.output      += truncation_note(trunc);
    result.truncated    = trunc.truncated;
    result.overflow_path = std::move(trunc.overflow_path);
    return result;
  }

  result.error = "bash_search: unknown action '" + *action +
                 "'; expected list_tags, search, or scan";
  return result;
}

void set_bash_search_index_path(std::string path) {
  // A rescan started under the old path must not land after the switch. The
  // liveness check keeps this from constructing an index nobody asked for:
  // sp calls this at startup, long before any bash_search call.
  wait_for_bash_search_rescan();
  index_path_override() = std::move(path);
}

void wait_for_bash_search_rescan() {
  if (g_index_live.load()) BashSearchIndex::instance().wait_for_rescan();
}

void reset_bash_search_index_for_test() {
  BashSearchIndex::instance().reset_for_test();
}

}  // namespace agent
