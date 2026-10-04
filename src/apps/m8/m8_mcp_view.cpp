#include <m8_mcp_view.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string_view>

namespace m8 {

namespace {

std::string sanitize(const std::string& text) {
  std::string out;
  for (const char c : text) {
    out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
  }
  return out;
}

const char* mark(mcp::ServerState state) {
  switch (state) {
    case mcp::ServerState::Connected:
      return "\xe2\x9c\x93";  // ✓
    case mcp::ServerState::Connecting:
      return "\xe2\x80\xa6";  // …
    case mcp::ServerState::Disabled:
      return "-";
    case mcp::ServerState::NeedsApproval:
    case mcp::ServerState::NeedsAuth:
      return "!";
    case mcp::ServerState::Failed:
    default:
      return "\xe2\x9c\x97";  // ✗
  }
}

std::string pad(const std::string& text, size_t width) {
  return text.size() >= width ? text + " " : text + std::string(width - text.size(), ' ');
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

// Shell-like words: whitespace separated, with "double" or 'single' quotes.
std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  bool in_word = false;
  char quote = 0;
  for (const char c : text) {
    if (quote != 0) {
      if (c == quote) {
        quote = 0;
      } else {
        current += c;
      }
      continue;
    }
    if (c == '"' or c == '\'') {
      quote = c;
      in_word = true;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (in_word) out.push_back(current);
      current.clear();
      in_word = false;
      continue;
    }
    current += c;
    in_word = true;
  }
  if (in_word) out.push_back(current);
  return out;
}

}  // namespace

std::string human_tokens(int64_t tokens) {
  if (tokens < 1000) return std::to_string(tokens);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(tokens) / 1000.0);
  return buf;
}

std::string mask_secret(const std::string& value) {
  static const std::string kDots = "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2";  // ••••
  // Literal text is shown only when it cannot be the secret itself: an auth
  // scheme in front of a reference, or punctuation between two.
  const auto shown = [](const std::string& literal) {
    for (const char* scheme : {"Bearer ", "bearer ", "Basic ", "Token ", "token "}) {
      if (literal == scheme) return true;
    }
    return std::none_of(literal.begin(), literal.end(), [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) != 0;
    });
  };
  std::string out;
  size_t at = 0;
  while (at < value.size()) {
    const size_t reference = value.find("${", at);
    const size_t literal_end = reference == std::string::npos ? value.size() : reference;
    if (literal_end > at) {
      const std::string literal = value.substr(at, literal_end - at);
      out += shown(literal) ? literal : kDots;
    }
    if (reference == std::string::npos) break;
    const size_t close = value.find('}', reference);
    if (close == std::string::npos) {
      out += kDots;
      break;
    }
    out += value.substr(reference, close + 1 - reference);
    at = close + 1;
  }
  return out;
}

std::string mcp_header(const mcp::Catalog& catalog) {
  size_t total = 0;
  size_t connected = 0;
  bool attention = false;
  for (const mcp::CatalogServer& server : catalog.servers) {
    if (server.state == mcp::ServerState::Disabled) continue;
    ++total;
    if (server.state == mcp::ServerState::Connected) ++connected;
    attention = attention or server.state == mcp::ServerState::NeedsApproval or
                server.state == mcp::ServerState::NeedsAuth;
  }
  if (total == 0) return std::string();
  return "mcp " + std::to_string(connected) + "/" + std::to_string(total) +
         (attention ? " !" : "");
}

std::string mcp_status_text(const mcp::Catalog& catalog,
                            const mcp::ToolSearchSettings& settings,
                            int64_t context_window, bool deferred,
                            const std::vector<std::string>& loaded) {
  if (catalog.servers.empty()) {
    return "No MCP servers are configured. Add one from a shell:\n"
           "  m8 mcp add <name> -- <command> [args...]   (a local stdio server)\n"
           "  m8 mcp add <name> <url>                    (a remote server)";
  }

  std::ostringstream out;
  out << "MCP servers (tool search " << settings.text() << ": "
      << (deferred ? "on" : "off") << " \xe2\x80\x94 " << catalog.tools.size()
      << " tools \xe2\x89\x88" << human_tokens(catalog.total_tokens) << " tok";
  if (settings.mode == mcp::ToolSearchSettings::Mode::Auto) {
    out << ", defers past " << human_tokens(settings.threshold_tokens(context_window))
        << " or " << mcp::ToolSearchSettings::kMaxUndeferredTools << " tools";
  }
  out << ")\n";

  for (const mcp::CatalogServer& server : catalog.servers) {
    out << "  " << mark(server.state) << " " << pad(server.name, 14)
        << pad(server.transport, 6);
    if (server.state == mcp::ServerState::Connected) {
      out << pad(server.protocol_version, 11) << server.tool_count
          << (server.tool_count == 1 ? " tool " : " tools ") << "\xe2\x89\x88"
          << human_tokens(server.schema_tokens) << "  " << server.scope;
      if (not server.info.name.empty()) {
        out << "  (" << server.info.name;
        if (not server.info.version.empty()) out << " " << server.info.version;
        out << ")";
      }
    } else if (server.state == mcp::ServerState::NeedsApproval) {
      out << "needs approval (" << server.scope << ": " << server.detail
          << ") \xe2\x80\x94 /mcp approve " << server.name;
    } else if (server.state == mcp::ServerState::NeedsAuth) {
      out << "needs login \xe2\x80\x94 /mcp login " << server.name;
    } else {
      out << mcp::state_name(server.state);
      if (not server.error.empty()) out << ": " << server.error;
    }
    out << "\n";
    if (server.state == mcp::ServerState::Failed and not server.stderr_tail.empty()) {
      std::string tail = server.stderr_tail;
      if (tail.size() > 300) tail = "..." + tail.substr(tail.size() - 300);
      out << "      stderr: " << tail << "\n";
    }
  }
  if (not loaded.empty()) {
    out << "loaded (root):";
    for (const std::string& name : loaded) out << " " << name;
    out << "\n";
  }
  out << "/mcp tools [server] \xc2\xb7 /mcp resources [server] \xc2\xb7 /mcp reconnect "
         "[server] \xc2\xb7 /mcp enable|disable <server> \xc2\xb7 /mcp approve "
         "<server>|--all \xc2\xb7 /mcp login|logout <server>";
  return out.str();
}

std::string mcp_tools_text(const mcp::Catalog& catalog, const std::string& server,
                           bool deferred, const std::vector<std::string>& loaded,
                           bool status) {
  std::ostringstream out;
  size_t shown = 0;
  for (const mcp::CatalogServer& entry : catalog.servers) {
    if (not server.empty() and entry.name != server) continue;
    if (entry.state != mcp::ServerState::Connected) continue;
    out << entry.name << " (" << entry.tool_count << " tools):\n";
    for (const mcp::CatalogTool& tool : catalog.tools) {
      if (tool.server != entry.name) continue;
      ++shown;
      const char* mark = tool.always_load                ? "always"
                         : not deferred                    ? "loaded"
                         : contains(loaded, tool.exposed) ? "loaded"
                                                          : "deferred";
      out << "  " << (status ? pad(mark, 9) : std::string()) << tool.exposed
          << "  \xe2\x89\x88" << human_tokens(tool.tokens);
      std::string description = tool.description;
      if (const size_t newline = description.find('\n'); newline != std::string::npos) {
        description.resize(newline);
      }
      if (description.size() > 80) description = description.substr(0, 77) + "...";
      if (not description.empty()) out << "  " << description;
      out << "\n";
    }
  }
  if (shown == 0) {
    return server.empty() ? "No connected MCP server has tools."
                          : "No connected MCP server named '" + server + "' has tools.";
  }
  return out.str();
}

std::string mcp_context_line(const mcp::Catalog& catalog, bool deferred,
                             const std::vector<std::string>& loaded) {
  if (catalog.tools.empty()) return std::string();
  int64_t loaded_tokens = 0;
  size_t loaded_count = 0;
  int64_t deferred_tokens = 0;
  size_t deferred_count = 0;
  for (const mcp::CatalogTool& tool : catalog.tools) {
    if (not deferred or tool.always_load or contains(loaded, tool.exposed)) {
      loaded_tokens += tool.tokens;
      ++loaded_count;
    } else {
      deferred_tokens += tool.tokens;
      ++deferred_count;
    }
  }
  std::string line = "mcp: " + std::to_string(catalog.tools.size()) + " tools \xc2\xb7 " +
                     (deferred ? "loaded " : "all loaded, ") +
                     std::to_string(loaded_count) + " (~" + human_tokens(loaded_tokens) + ")";
  if (deferred) {
    line += " \xc2\xb7 deferred " + std::to_string(deferred_count) + " (~" +
            human_tokens(deferred_tokens) + " saved per call)";
  }
  return line;
}

std::string pending_approval_text(const mcp::Catalog& catalog) {
  std::string list;
  size_t count = 0;
  for (const mcp::CatalogServer& server : catalog.servers) {
    if (server.state != mcp::ServerState::NeedsApproval) continue;
    ++count;
    list += "\n  " + server.name + " (" + server.scope + "): " + server.detail;
  }
  if (count == 0) return std::string();
  return "mcp: " + std::to_string(count) +
         (count == 1 ? " server from this workspace needs" : " servers from this workspace need") +
         " your approval before it runs. Review the command" +
         (count == 1 ? "" : "s") + ", then /mcp approve <name> (or --all):" + list;
}

std::string prompt_command_name(const std::string& server, const std::string& prompt) {
  return "mcp__" + sanitize(server) + "__" + sanitize(prompt);
}

std::string prompt_usage(const std::string& server, const mcp::PromptInfo& prompt) {
  std::string usage = "/" + prompt_command_name(server, prompt.name);
  for (const mcp::PromptArgument& argument : prompt.arguments) {
    usage += argument.required ? " <" + argument.name + ">" : " [" + argument.name + "]";
  }
  return usage;
}

std::string prompt_help_text(const mcp::Catalog& catalog) {
  std::string out;
  for (const mcp::CatalogServer& server : catalog.servers) {
    if (server.state != mcp::ServerState::Connected) continue;
    for (const mcp::PromptInfo& prompt : server.prompts) {
      out += "\n" + prompt_usage(server.name, prompt);
      const std::string& description =
          prompt.description.empty() ? prompt.title : prompt.description;
      if (not description.empty()) out += " \xe2\x80\x94 " + description;
    }
  }
  return out.empty() ? out : "\nMCP prompt commands:" + out;
}

std::optional<PromptCommand> parse_prompt_command(const std::string& line,
                                                  const mcp::Catalog& catalog) {
  if (line.rfind("/mcp__", 0) != 0) return std::nullopt;
  const size_t space = line.find_first_of(" \t");
  const std::string name = line.substr(1, space == std::string::npos ? std::string::npos : space - 1);
  std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
  rest.erase(0, rest.find_first_not_of(" \t"));

  for (const mcp::CatalogServer& server : catalog.servers) {
    if (server.state != mcp::ServerState::Connected) continue;
    for (const mcp::PromptInfo& prompt : server.prompts) {
      if (prompt_command_name(server.name, prompt.name) != name) continue;

      PromptCommand command;
      command.server = server.name;
      command.prompt = prompt.name;
      command.arguments = util::JsonValue::object();

      // One declared argument takes the whole rest of the line, so a prompt
      // like "review <code>" needs no quoting.
      if (prompt.arguments.size() == 1 and not rest.empty() and
          rest.rfind(prompt.arguments[0].name + "=", 0) != 0) {
        command.arguments.set(prompt.arguments[0].name, rest);
      } else {
        size_t next_positional = 0;
        for (const std::string& word : words(rest)) {
          const size_t equals = word.find('=');
          bool named = false;
          if (equals != std::string::npos and equals > 0) {
            const std::string key = word.substr(0, equals);
            for (const mcp::PromptArgument& argument : prompt.arguments) {
              if (argument.name == key) {
                command.arguments.set(key, word.substr(equals + 1));
                named = true;
              }
            }
          }
          if (named) continue;
          while (next_positional < prompt.arguments.size() and
                 command.arguments.contains(prompt.arguments[next_positional].name)) {
            ++next_positional;
          }
          if (next_positional >= prompt.arguments.size()) {
            command.error = "too many arguments. Usage: " + prompt_usage(server.name, prompt);
            return command;
          }
          command.arguments.set(prompt.arguments[next_positional++].name, word);
        }
      }
      for (const mcp::PromptArgument& argument : prompt.arguments) {
        if (argument.required and not command.arguments.contains(argument.name)) {
          command.error = "missing <" + argument.name + ">. Usage: " +
                          prompt_usage(server.name, prompt);
          break;
        }
      }
      return command;
    }
  }
  return std::nullopt;
}

std::string server_config_text(const mcp::ServerConfig& config) {
  std::ostringstream out;
  out << config.name << " (" << mcp::scope_name(config.scope) << ", " << config.source
      << ")\n";
  out << "  type: " << config.type << "\n";
  if (config.type == "http") {
    out << "  url: " << config.url << "\n";
    for (const auto& [name, value] : config.headers) {
      out << "  header " << name << ": " << mask_secret(value) << "\n";
    }
    if (not config.oauth.client_id.empty()) {
      out << "  oauth client: " << config.oauth.client_id << "\n";
    }
  } else {
    out << "  command: " << config.command;
    for (const std::string& arg : config.args) out << " " << arg;
    out << "\n";
    for (const auto& [name, value] : config.env) {
      out << "  env " << name << "=" << mask_secret(value) << "\n";
    }
    if (not config.cwd.empty()) out << "  cwd: " << config.cwd << "\n";
  }
  if (config.disabled) out << "  disabled\n";
  if (config.always_load) out << "  alwaysLoad: tools never deferred\n";
  if (not config.enabled_tools.empty()) {
    out << "  enabledTools:";
    for (const std::string& tool : config.enabled_tools) out << " " << tool;
    out << "\n";
  }
  if (not config.disabled_tools.empty()) {
    out << "  disabledTools:";
    for (const std::string& tool : config.disabled_tools) out << " " << tool;
    out << "\n";
  }
  if (config.protocol != "auto") out << "  protocol: " << config.protocol << "\n";
  if (not config.problem.empty()) out << "  problem: " << config.problem << "\n";
  return out.str();
}

std::string tool_display_name(const std::string& name) {
  constexpr std::string_view kPrefix = "mcp__";
  if (name.rfind(kPrefix, 0) != 0) return name;
  // Server names cannot contain "__", so the first one after the prefix ends it.
  const size_t split = name.find("__", kPrefix.size());
  if (split == std::string::npos or split == kPrefix.size() or split + 2 >= name.size()) {
    return name;
  }
  return name.substr(kPrefix.size(), split - kPrefix.size()) + " \xe2\x80\xba " +
         name.substr(split + 2);
}

}  // namespace m8
