#include <core/tools.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <simdjson.h>

#include <core/json_util.h>
#include <core/tools_util.h>

namespace agent {

namespace {

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
    // m8trixparrot file tools exposed as standalone CLI utilities.
    {"tool_read",    {"file", "text"}},
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

// ───────────────────────── whatis line parser ─────────────────────────────────

// Parses one whatis output line such as:
//   ls (1)               - list directory contents
//   grep (1)             -- print lines matching a pattern
// Returns {name, description}; both empty if the line is not in that form.
std::pair<std::string, std::string> parse_whatis_line(std::string_view line) {
  // Find " - " or " -- " separator.
  auto sep = line.find(" -- ");
  size_t desc_offset = 4;
  if (sep == std::string_view::npos) {
    sep = line.find(" - ");
    desc_offset = 3;
  }
  if (sep == std::string_view::npos) return {"", ""};

  const std::string desc(line.substr(sep + desc_offset));
  std::string_view name_part = line.substr(0, sep);
  // Strip trailing whitespace and the section "(1)" suffix.
  const auto sp = name_part.find(' ');
  const auto lp = name_part.find('(');
  const size_t end = std::min({sp, lp, name_part.size()});
  return {std::string(name_part.substr(0, end)), desc};
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

struct TagQuery {
  std::vector<std::string> tokens;
  size_t pos = 0;
  std::string error;

  explicit TagQuery(const std::string& query) {
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

  bool at_end() const { return pos >= tokens.size(); }
  const std::string& peek() const { return tokens[pos]; }
  std::string consume() { return tokens[pos++]; }

  std::optional<bool> eval(const std::set<std::string>& tags) {
    auto v = parse_or(tags);
    if (!error.empty()) return {};
    if (!at_end()) { error = "unexpected token '" + peek() + "'"; return {}; }
    return v;
  }

private:
  std::optional<bool> parse_or(const std::set<std::string>& tags) {
    auto left = parse_and(tags);
    if (!left) return {};
    while (!at_end() && peek() == "OR") {
      consume();
      auto right = parse_and(tags);
      if (!right) return {};
      *left = *left || *right;
    }
    return left;
  }

  std::optional<bool> parse_and(const std::set<std::string>& tags) {
    auto left = parse_atom(tags);
    if (!left) return {};
    while (!at_end() && peek() == "AND") {
      consume();
      auto right = parse_atom(tags);
      if (!right) return {};
      *left = *left && *right;
    }
    return left;
  }

  std::optional<bool> parse_atom(const std::set<std::string>& tags) {
    if (at_end()) { error = "unexpected end of query"; return {}; }
    if (peek() == "(") {
      consume();
      auto inner = parse_or(tags);
      if (!inner) return {};
      if (at_end() || peek() != ")") { error = "missing closing ')'"; return {}; }
      consume();
      return inner;
    }
    if (peek() == ")") { error = "unexpected ')'"; return {}; }
    const std::string tag = consume();
    return std::optional<bool>(tags.count(tag) > 0);
  }
};

// ─────────────────────────── index singleton ──────────────────────────────────

class BashSearchIndex {
public:
  static BashSearchIndex& instance() {
    static BashSearchIndex inst;
    return inst;
  }

  // Loads the index if not already loaded (triggers a scan if no file exists).
  void ensure_loaded() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mLoaded) {
      load_locked();
      mLoaded = true;
    }
  }

  // Rebuilds the index from PATH + whatis, saves it, and returns a summary.
  std::string scan() {
    std::lock_guard<std::mutex> lock(mMutex);
    mIndex.clear();
    scan_locked();
    save_locked();
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
    std::lock_guard<std::mutex> lock(mMutex);
    std::vector<CommandEntry> results;
    for (const auto& [_, entry] : mIndex) {
      const std::set<std::string> tag_set(entry.tags.begin(), entry.tags.end());
      TagQuery tq(query);
      const auto match = tq.eval(tag_set);
      if (!tq.error.empty()) { error = tq.error; return {}; }
      if (match && *match) results.push_back(entry);
    }
    return results;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mIndex.size();
  }

private:
  mutable std::mutex mMutex;
  std::map<std::string, CommandEntry> mIndex;
  bool mLoaded = false;

  std::string index_path() const {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.m8trix/bash_search_index.json";
  }

  // ── must be called under mMutex ──

  void load_locked() {
    const auto text = read_file(index_path());
    if (!text) { scan_locked(); save_locked(); return; }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(*text);
    simdjson::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return;
    simdjson::ondemand::object root;
    if (doc.get_object().get(root)) return;

    simdjson::ondemand::array arr;
    if (root["commands"].get_array().get(arr)) return;

    for (auto item : arr) {
      simdjson::ondemand::object obj;
      if (item.get_object().get(obj)) continue;
      CommandEntry e;
      e.name        = string_field(obj, "name");
      e.description = string_field(obj, "description");
      e.usage       = string_field(obj, "usage");
      e.tags        = string_array_field(obj, "tags");
      if (!e.name.empty()) mIndex[e.name] = std::move(e);
    }
  }

  void save_locked() const {
    const std::string path = index_path();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    if (ec) return;

    JsonWriter w;
    w.begin_object().key("commands").begin_array();
    for (const auto& [_, e] : mIndex) {
      w.begin_object()
          .field("name", e.name)
          .field("description", e.description)
          .field("usage", e.usage)
          .field("tags", e.tags)
          .end_object();
    }
    w.end_array().end_object();

    std::ofstream out(path);
    if (out) out << w.str();
  }

  void scan_locked() {
    // 1. Enumerate executables in PATH, deduplicating by name.
    const char* path_env = std::getenv("PATH");
    if (!path_env) return;

    std::set<std::string> seen;
    std::vector<std::string> names;
    std::istringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
      if (dir.empty()) continue;
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code ec2;
        if (!std::filesystem::is_regular_file(e.path(), ec2)) continue;
        const auto perms = e.status(ec2).permissions();
        if (ec2) continue;
        const bool exec =
            (perms & std::filesystem::perms::owner_exec)  != std::filesystem::perms::none ||
            (perms & std::filesystem::perms::group_exec)  != std::filesystem::perms::none ||
            (perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none;
        if (!exec) continue;
        const std::string name = e.path().filename().string();
        if (seen.insert(name).second) names.push_back(name);
      }
    }

    // 2. Batch-query whatis for all names at once.
    std::map<std::string, std::string> whatis_desc;
    if (!names.empty()) {
      std::string cmd = "whatis";
      for (const auto& n : names) cmd += " " + shell_quote(n);
      cmd += " 2>/dev/null";
      const std::string out = run_shell_capture(cmd);
      std::istringstream oss(out);
      std::string line;
      while (std::getline(oss, line)) {
        auto [name, desc] = parse_whatis_line(line);
        if (!name.empty() && !whatis_desc.count(name)) whatis_desc[name] = desc;
      }
    }

    // 3. Build index entries.
    for (const auto& name : names) {
      CommandEntry entry;
      entry.name        = name;
      entry.description = whatis_desc.count(name) ? whatis_desc.at(name) : "";
      entry.usage       = "";
      entry.tags        = assign_tags(name, entry.description);
      mIndex[name]      = std::move(entry);
    }
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

}  // namespace agent
