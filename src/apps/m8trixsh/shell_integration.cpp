#include <shell_integration.h>

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace m8sh {

namespace {

// Wraps `s` in single quotes for embedding in a generated zsh script. Only `'`
// needs care: close the quote, emit an escaped quote, reopen.
std::string sq(std::string_view s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

bool write_file(const std::string& path, std::string_view content) {
  FILE* file = std::fopen(path.c_str(), "w");
  if (file == nullptr) return false;
  const size_t written = std::fwrite(content.data(), 1, content.size(), file);
  return std::fclose(file) == 0 and written == content.size();
}

std::string with_default(std::string_view value, std::string_view fallback) {
  return std::string(value.empty() ? fallback : value);
}

// The fixed part of the integration snippet. The four `__M8SH_*` parameters it
// reads are assigned just above it, so this text has nothing to interpolate.
constexpr std::string_view kSnippetBody = R"zsh(
autoload -Uz add-zsh-hook

__m8sh_mode=shell
__m8sh_read_mode() {
  local m=shell
  [[ -r "$__M8SH_MODE_FILE" ]] && IFS= read -r m < "$__M8SH_MODE_FILE"
  __m8sh_mode=${m//[^a-z-]/}
  [[ -n "$__m8sh_mode" ]] || __m8sh_mode=shell
}

# m8trixsh's own branch segment - no oh-my-zsh / theme dependency.
__m8sh_git_segment=''
__m8sh_update_git() {
  __m8sh_git_segment=''
  command git rev-parse --is-inside-work-tree &>/dev/null || return
  local ref
  ref=$(command git symbolic-ref --quiet --short HEAD 2>/dev/null) ||
    ref=$(command git rev-parse --short HEAD 2>/dev/null) || return
  local dirty=''
  command git diff --no-ext-diff --quiet &>/dev/null || dirty='*'
  command git diff --no-ext-diff --cached --quiet &>/dev/null || dirty='*'
  __m8sh_git_segment="git:(${ref})${dirty}"
}

__m8sh_render_prompt() {
  local tag
  case $__m8sh_mode in
    ai)     tag=$__M8SH_TAG_AI ;;
    ai-ask) tag=$__M8SH_TAG_ASK ;;
    *)      tag=$__M8SH_TAG_SHELL ;;
  esac
  local p=$__M8SH_FMT
  p=${p//'%tag'/$tag}
  p=${p//'%git'/$__m8sh_git_segment}
  PROMPT=$p
}

__m8sh_precmd() {
  __m8sh_read_mode
  __m8sh_update_git
  __m8sh_render_prompt
  printf '\033]7;file://%s%s\007' "${HOST-}" "$PWD"
}
add-zsh-hook precmd __m8sh_precmd

# Repaint the prompt when m8trixsh flips the mode at an idle prompt.
__m8sh_redraw() {
  __m8sh_read_mode
  __m8sh_render_prompt
  zle reset-prompt
}
zle -N __m8sh_redraw
bindkey '\e[218~' __m8sh_redraw

# Enter: run the line in shell mode; hand it to the agent in ai mode.
__m8sh_accept() {
  __m8sh_read_mode
  if [[ ( $__m8sh_mode != ai && $__m8sh_mode != ai-ask ) || -z $BUFFER ]]; then
    zle .accept-line
    return
  fi
  local encoded
  encoded=$(printf '%s' "$BUFFER" | base64 | tr -d '\n')
  printf '\033]5171;%s\007' "$encoded"
  zle send-break
}
zle -N __m8sh_accept
bindkey -M main '^M' __m8sh_accept
bindkey -M main '^J' __m8sh_accept
)zsh";

}  // namespace

bool shell_is_zsh(std::string_view resolved_shell) {
  std::string_view base = resolved_shell;
  if (const size_t slash = base.find_last_of('/');
      slash != std::string_view::npos) {
    base = base.substr(slash + 1);
  }
  if (not base.empty() and base.front() == '-') base.remove_prefix(1);
  return base == "zsh";
}

ShellIntegration::ShellIntegration(const PromptConfig& prompt) {
  const char* tmp_env = std::getenv("TMPDIR");
  std::string root =
      (tmp_env != nullptr and *tmp_env != '\0') ? tmp_env : "/tmp";
  while (root.size() > 1 and root.back() == '/') root.pop_back();

  std::string tmpl = root + "/m8trixsh-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) {
    mError = "mkdtemp(" + tmpl + "): " + std::strerror(errno);
    return;
  }
  mDir = buf.data();
  mModeFile = mDir + "/mode";

  const char* home_env = std::getenv("HOME");
  const char* zdot_env = std::getenv("ZDOTDIR");
  const std::string real =
      (zdot_env != nullptr and *zdot_env != '\0')   ? zdot_env
      : (home_env != nullptr and *home_env != '\0') ? home_env
                                                    : "/";

  const std::string fmt = with_default(prompt.format, kDefaultPromptFormat);
  const std::string shell_tag = with_default(prompt.shell_tag, kDefaultShellTag);
  const std::string ai_tag = with_default(prompt.ai_tag, kDefaultAiTag);
  const std::string ask_tag = with_default(prompt.ask_tag, kDefaultAskTag);

  const std::string qdir = sq(mDir);
  const std::string qreal = sq(real);
  const auto real_rc = [&](const char* name) { return sq(real + name); };

  bool ok = true;
  ok = ok and write_file(mModeFile, "shell\n");
  ok = ok and write_file(mDir + "/integration.zsh",
                         "__M8SH_MODE_FILE=" + sq(mModeFile) +
                             "\n__M8SH_FMT=" + sq(fmt) + "\n__M8SH_TAG_SHELL=" +
                             sq(shell_tag) + "\n__M8SH_TAG_AI=" + sq(ai_tag) +
                             "\n__M8SH_TAG_ASK=" + sq(ask_tag) + "\n" +
                             std::string(kSnippetBody));
  ok = ok and write_file(mDir + "/.zshenv",
                         "[[ -f " + real_rc("/.zshenv") + " ]] && source " +
                             real_rc("/.zshenv") + "\nZDOTDIR=" + qdir + "\n");
  ok = ok and write_file(mDir + "/.zprofile",
                         "[[ -f " + real_rc("/.zprofile") + " ]] && source " +
                             real_rc("/.zprofile") + "\nZDOTDIR=" + qdir + "\n");
  ok = ok and write_file(mDir + "/.zshrc",
                         "ZDOTDIR=" + qreal + "\n[[ -f " + real_rc("/.zshrc") +
                             " ]] && source " + real_rc("/.zshrc") +
                             "\nsource " + sq(mDir + "/integration.zsh") + "\n");
  ok = ok and write_file(mDir + "/.zlogin",
                         "ZDOTDIR=" + qreal + "\n[[ -f " + real_rc("/.zlogin") +
                             " ]] && source " + real_rc("/.zlogin") + "\n");

  if (not ok) {
    mError = "could not write the integration files under " + mDir;
    return;
  }
  mOk = true;
}

ShellIntegration::~ShellIntegration() {
  if (mDir.empty()) return;
  std::error_code ec;
  std::filesystem::remove_all(mDir, ec);
}

void ShellIntegration::set_mode(std::string_view mode) const {
  if (mModeFile.empty()) return;
  FILE* file = std::fopen(mModeFile.c_str(), "w");
  if (file == nullptr) return;
  std::fwrite(mode.data(), 1, mode.size(), file);
  std::fputc('\n', file);
  std::fclose(file);
}

}  // namespace m8sh
