#include <core/mcp/config.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>

#include <core/mcp/catalog.h>
#include <core/util/sha256.h>

namespace mcp {

namespace {

using util::JsonValue;

std::vector<std::string> string_list(const JsonValue& value) {
  std::vector<std::string> out;
  for (const JsonValue& item : value.items()) {
    if (item.is_string()) out.push_back(item.as_string());
  }
  return out;
}

// env and headers values: strings, with numbers and booleans spelled out
// (people write {"PORT": 8080}).
std::vector<std::pair<std::string, std::string>> string_map(const JsonValue& value) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const JsonValue::Member& member : value.members()) {
    const JsonValue& v = member.value;
    if (v.is_string()) {
      out.emplace_back(member.key, v.as_string());
    } else if (v.is_number() or v.is_bool()) {
      out.emplace_back(member.key, v.dump());
    }
  }
  return out;
}

// http(s)://..., or a ${VAR} that will expand to the whole URL.
bool http_url(std::string_view url) {
  std::string head(url.substr(0, 8));
  for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return head.rfind("http://", 0) == 0 or head.rfind("https://", 0) == 0 or
         url.rfind("${", 0) == 0;
}

ServerConfig parse_entry(const std::string& name, const JsonValue& value,
                         Scope scope, const std::string& source) {
  ServerConfig server;
  server.name = name;
  server.scope = scope;
  server.source = source;
  server.raw = value;

  if (std::string problem = server_name_problem(name); not problem.empty()) {
    server.problem = problem;
    return server;
  }
  if (not value.is_object()) {
    server.problem = "its entry is not a JSON object";
    return server;
  }

  server.command = value.get("command").as_string();
  server.args = string_list(value.get("args"));
  server.env = string_map(value.get("env"));
  server.cwd = value.get("cwd").as_string();
  server.inherit_env = value.get("inheritEnv").as_bool(false);
  server.url = value.get("url").as_string();
  server.headers = string_map(value.get("headers"));

  const JsonValue& oauth = value.get("oauth");
  server.oauth.client_id = oauth.get("clientId").as_string();
  server.oauth.client_secret = oauth.get("clientSecret").as_string();
  if (oauth.get("scopes").is_array()) {
    server.oauth.scopes = string_list(oauth.get("scopes"));
  } else if (const std::string& scopes = oauth.get("scopes").as_string();
             not scopes.empty()) {
    std::istringstream words(scopes);
    for (std::string word; words >> word;) server.oauth.scopes.push_back(word);
  }
  server.oauth.callback_port = static_cast<int>(oauth.get("callbackPort").as_int(0));

  server.disabled = value.get("disabled").as_bool(false);
  server.always_load = value.get("alwaysLoad").as_bool(false);
  server.enabled_tools = string_list(value.get("enabledTools"));
  server.disabled_tools = string_list(value.get("disabledTools"));
  server.timeout_ms = value.get("timeout").as_int(0);
  server.startup_timeout_ms = value.get("startupTimeout").as_int(0);
  server.protocol = value.get("protocol").string_or("auto");
  if (server.protocol != "auto" and server.protocol != "modern" and
      server.protocol != "legacy") {
    server.problem = "\"protocol\" must be \"auto\", \"modern\" or \"legacy\"";
    return server;
  }

  std::string type = value.get("type").as_string();
  if (type == "streamable-http" or type == "streamableHttp" or type == "streamable_http") {
    type = "http";
  }
  if (type == "sse") {
    server.problem =
        "the deprecated HTTP+SSE transport is not supported; point \"url\" at "
        "the server's Streamable HTTP endpoint (often ending in /mcp) and set "
        "\"type\": \"http\"";
    return server;
  }
  if (type.empty()) type = not server.command.empty() ? "stdio" : (not server.url.empty() ? "http" : "");
  server.type = type;
  if (type == "stdio" and server.command.empty()) {
    server.problem = "a stdio server needs a \"command\"";
  } else if (type == "http" and server.url.empty()) {
    server.problem = "an http server needs a \"url\"";
  } else if (type == "http" and not http_url(server.url)) {
    server.problem = "\"url\" must start with http:// or https://";
  } else if (type != "stdio" and type != "http") {
    server.problem = type.empty() ? "it needs a \"command\" (stdio) or a \"url\" (http)"
                                  : "unknown \"type\" \"" + type + "\"";
  }
  return server;
}

std::string read_text(const std::string& path, bool& exists) {
  std::ifstream in(path, std::ios::binary);
  exists = static_cast<bool>(in);
  if (not exists) return std::string();
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string real_path(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
  return ec ? path : resolved.string();
}

// The file as an object with an mcpServers object inside, or an error.
bool load_for_edit(const std::string& path, JsonValue& root, std::string& error) {
  bool exists = false;
  const std::string text = read_text(path, exists);
  root = JsonValue::object();
  if (not exists or text.find_first_not_of(" \t\r\n") == std::string::npos) {
    root.set("mcpServers", JsonValue::object());
    return true;
  }
  std::string parse_error;
  std::optional<JsonValue> parsed = JsonValue::parse(text, &parse_error);
  if (not parsed or not parsed->is_object()) {
    error = path + " is not a JSON object" +
            (parse_error.empty() ? std::string() : " (" + parse_error + ")") +
            "; fix or remove it first";
    return false;
  }
  root = std::move(*parsed);
  if (not root.get("mcpServers").is_object()) {
    if (root.contains("mcpServers")) {
      error = path + ": \"mcpServers\" is not an object";
      return false;
    }
    root.set("mcpServers", JsonValue::object());
  }
  return true;
}

}  // namespace

const char* scope_name(Scope scope) {
  switch (scope) {
    case Scope::User:
      return "user";
    case Scope::Shared:
      return "shared";
    case Scope::Project:
    default:
      return "project";
  }
}

std::string expand_variables(std::string_view text, std::string& missing) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '$' and i + 1 < text.size() and text[i + 1] == '{') {
      const size_t close = text.find('}', i + 2);
      if (close != std::string_view::npos) {
        const std::string_view inner = text.substr(i + 2, close - i - 2);
        const size_t fallback_at = inner.find(":-");
        const std::string name(inner.substr(0, fallback_at));
        const char* value = name.empty() ? nullptr : std::getenv(name.c_str());
        if (value != nullptr and *value != '\0') {
          out += value;
        } else if (fallback_at != std::string_view::npos) {
          out += inner.substr(fallback_at + 2);
        } else if (missing.empty()) {
          missing = name;
        }
        i = close + 1;
        continue;
      }
    }
    out += text[i++];
  }
  return out;
}

bool expand_server(const ServerConfig& in, ServerConfig& out, std::string& error) {
  out = in;
  std::string missing;
  out.command = expand_variables(in.command, missing);
  for (std::string& arg : out.args) arg = expand_variables(arg, missing);
  for (auto& [name, value] : out.env) value = expand_variables(value, missing);
  out.cwd = expand_variables(in.cwd, missing);
  out.url = expand_variables(in.url, missing);
  for (auto& [name, value] : out.headers) value = expand_variables(value, missing);
  out.oauth.client_id = expand_variables(in.oauth.client_id, missing);
  out.oauth.client_secret = expand_variables(in.oauth.client_secret, missing);
  if (not missing.empty()) {
    error = "the environment variable " + missing +
            " is not set (it is used in this server's config)";
    return false;
  }
  return true;
}

std::vector<ServerConfig> parse_config(std::string_view text, Scope scope,
                                       const std::string& source,
                                       std::vector<std::string>& warnings) {
  std::vector<ServerConfig> servers;
  if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) return servers;

  std::string error;
  std::optional<JsonValue> root = JsonValue::parse(text, &error);
  if (not root or not root->is_object()) {
    warnings.push_back(source + " is not a JSON object" +
                       (error.empty() ? std::string() : " (" + error + ")") +
                       "; its MCP servers were not loaded");
    return servers;
  }
  const JsonValue& entries = root->get("mcpServers");
  if (not entries.is_object()) {
    if (root->contains("mcpServers")) {
      warnings.push_back(source + ": \"mcpServers\" is not an object");
    }
    return servers;
  }
  for (const JsonValue::Member& member : entries.members()) {
    servers.push_back(parse_entry(member.key, member.value, scope, source));
    if (not servers.back().problem.empty()) {
      warnings.push_back("MCP server '" + member.key + "' in " + source + ": " +
                         servers.back().problem);
    }
  }
  return servers;
}

LoadedConfig load_config(const ConfigFiles& files) {
  LoadedConfig loaded;
  std::map<std::string, size_t> index;

  const auto read = [&](const std::string& path, Scope scope) {
    if (path.empty()) return;
    bool exists = false;
    const std::string text = read_text(path, exists);
    if (not exists) return;
    for (ServerConfig& server : parse_config(text, scope, path, loaded.warnings)) {
      // Later files are more specific and replace an earlier entry in place.
      if (auto it = index.find(server.name); it != index.end()) {
        loaded.servers[it->second] = std::move(server);
      } else {
        index[server.name] = loaded.servers.size();
        loaded.servers.push_back(std::move(server));
      }
    }
  };

  read(files.user, Scope::User);
  read(files.shared, Scope::Shared);
  if (files.project.empty() or files.user.empty() or
      real_path(files.project) != real_path(files.user)) {
    read(files.project, Scope::Project);
  }
  return loaded;
}

std::string config_fingerprint(const ServerConfig& server) {
  // Only what decides what runs and where requests go.
  static constexpr const char* kKeys[] = {"type",    "command", "args", "env",
                                          "cwd",     "url",     "headers",
                                          "oauth",   "inheritEnv"};
  JsonValue canonical = JsonValue::object();
  canonical.set("type", server.type);
  for (const char* key : kKeys) {
    if (std::string_view(key) == "type") continue;
    if (const JsonValue* value = server.raw.find(key)) canonical.set(key, *value);
  }
  // Sorted keys at every level, so a reformatted file keeps its approval.
  const std::function<JsonValue(const JsonValue&)> sorted = [&](const JsonValue& value) {
    if (value.is_object()) {
      std::vector<JsonValue::Member> members = value.members();
      std::sort(members.begin(), members.end(),
                [](const JsonValue::Member& a, const JsonValue::Member& b) {
                  return a.key < b.key;
                });
      JsonValue out = JsonValue::object();
      for (const JsonValue::Member& member : members) {
        out.set(member.key, sorted(member.value));
      }
      return out;
    }
    if (value.is_array()) {
      JsonValue out = JsonValue::array();
      for (const JsonValue& item : value.items()) out.push_back(sorted(item));
      return out;
    }
    return value;
  };
  return util::sha256_hex(sorted(canonical).dump());
}

bool write_file_atomically(const std::string& path, const std::string& text,
                           bool private_file, std::string& error) {
  std::error_code ec;
  const std::filesystem::path target(path);
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path(), ec);
  }
  const std::string temp = path + ".tmp-" + std::to_string(::getpid());
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        private_file ? 0600 : 0644);
  if (fd < 0) {
    error = "could not write " + path + ": " + std::strerror(errno);
    return false;
  }
  size_t written = 0;
  while (written < text.size()) {
    const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) {
      error = "could not write " + path + ": " + std::strerror(errno);
      ::close(fd);
      ::unlink(temp.c_str());
      return false;
    }
    written += static_cast<size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  if (::rename(temp.c_str(), path.c_str()) != 0) {
    error = "could not replace " + path + ": " + std::strerror(errno);
    ::unlink(temp.c_str());
    return false;
  }
  return true;
}

EditResult add_server(const std::string& path, const std::string& name,
                      const util::JsonValue& entry, bool replace) {
  EditResult result;
  if (std::string problem = server_name_problem(name); not problem.empty()) {
    result.error = problem;
    return result;
  }
  JsonValue root;
  if (not load_for_edit(path, root, result.error)) return result;
  JsonValue& servers = root["mcpServers"];
  if (servers.contains(name) and not replace) {
    result.error = "an MCP server named '" + name + "' already exists in " + path;
    return result;
  }
  servers.set(name, entry);
  result.ok = write_file_atomically(path, root.dump_pretty() + "\n",
                                    /*private_file=*/true, result.error);
  return result;
}

EditResult remove_server(const std::string& path, const std::string& name) {
  EditResult result;
  JsonValue root;
  if (not load_for_edit(path, root, result.error)) return result;
  if (not root["mcpServers"].erase(name)) {
    result.error = "no MCP server named '" + name + "' in " + path;
    return result;
  }
  result.ok = write_file_atomically(path, root.dump_pretty() + "\n",
                                    /*private_file=*/true, result.error);
  return result;
}

util::JsonValue stdio_entry(const std::string& command,
                            const std::vector<std::string>& args,
                            const std::vector<std::pair<std::string, std::string>>& env) {
  JsonValue entry = JsonValue::object();
  entry.set("type", "stdio");
  entry.set("command", command);
  JsonValue array = JsonValue::array();
  for (const std::string& arg : args) array.push_back(arg);
  entry.set("args", std::move(array));
  if (not env.empty()) {
    JsonValue object = JsonValue::object();
    for (const auto& [name, value] : env) object.set(name, value);
    entry.set("env", std::move(object));
  }
  return entry;
}

util::JsonValue http_entry(const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers) {
  JsonValue entry = JsonValue::object();
  entry.set("type", "http");
  entry.set("url", url);
  if (not headers.empty()) {
    JsonValue object = JsonValue::object();
    for (const auto& [name, value] : headers) object.set(name, value);
    entry.set("headers", std::move(object));
  }
  return entry;
}

}  // namespace mcp
