#ifndef M8_MCP_CLI_H
#define M8_MCP_CLI_H

#include <iosfwd>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include <m8_paths.h>

namespace m8 {

// `m8 mcp ...`: installing and managing MCP servers from a shell, the way
// `claude mcp` does it.
//
//   m8 mcp add [-s project|user|shared] [-t stdio|http] [-e K=V]... [-H "K: V"]...
//              <name> <command|url> [-- args...]
//   m8 mcp add-json <name> '<json>' [-s ...]
//   m8 mcp list | get <name> | remove <name> [-s ...]
//   m8 mcp enable <name> | disable <name>
//   m8 mcp approve <name> | --all
//   m8 mcp login <name> | logout <name>
//
// Everything that changes which servers run (add, add-json, remove, enable,
// approve, import) refuses inside an m8 agent's shell — m8 marks its children
// with M8_AGENT_SHELL — because which programs m8 starts on the user's behalf
// is the user's decision. approve also wants a terminal to ask on. Both are
// guardrails in the spirit of protected paths, not a sandbox.
struct McpCli {
  // Declares the subcommands under `app`; call before parsing.
  void declare(CLI::App& app);
  bool parsed() const;

  // Runs whichever subcommand was given and returns the exit status.
  // `agent_shell` is whether M8_AGENT_SHELL was set when m8 started.
  int run(const M8Paths& paths, bool agent_shell, std::ostream& out,
          std::ostream& err, std::istream& in);

  // Whether stdin and stdout are a terminal. Tests set it.
  bool interactive = false;

private:
  int add(const M8Paths& paths, std::ostream& out, std::ostream& err);
  int add_json(const M8Paths& paths, std::ostream& out, std::ostream& err);
  int list(const M8Paths& paths, std::ostream& out, std::ostream& err);
  int get(const M8Paths& paths, std::ostream& out, std::ostream& err);
  int remove(const M8Paths& paths, std::ostream& out, std::ostream& err);
  int set_enabled(const M8Paths& paths, bool enabled, std::ostream& out,
                  std::ostream& err);
  int approve(const M8Paths& paths, std::ostream& out, std::ostream& err,
              std::istream& in);
  int login(const M8Paths& paths, bool logout, std::ostream& out,
            std::ostream& err);

  CLI::App* mMcp = nullptr;
  CLI::App* mAdd = nullptr;
  CLI::App* mAddJson = nullptr;
  CLI::App* mList = nullptr;
  CLI::App* mGet = nullptr;
  CLI::App* mRemove = nullptr;
  CLI::App* mEnable = nullptr;
  CLI::App* mDisable = nullptr;
  CLI::App* mApprove = nullptr;
  CLI::App* mLogin = nullptr;
  CLI::App* mLogout = nullptr;

  std::string mName;
  std::string mTarget;
  std::vector<std::string> mArgs;
  std::string mScope = "project";
  std::string mTransport;
  std::vector<std::string> mEnv;
  std::vector<std::string> mHeaders;
  std::string mJson;
  bool mAll = false;
};

// The key approvals are stored under: the workspace root's real path.
std::string workspace_key(const M8Paths& paths);

}  // namespace m8

#endif  // M8_MCP_CLI_H
