# Hook Events Reference

## Communication Protocol

Hooks communicate with Claude Code through stdin/stdout/stderr and exit codes.

**Input**: JSON on stdin with event-specific fields (see schemas below).

**Output / exit codes:**

| Exit code | Effect |
|---|---|
| `0` | Allow the action. Stdout (if any) becomes context for Claude. |
| `2` | **Block** the action. Stderr message is shown to Claude as feedback. |
| Any other | Action proceeds; stderr is logged but not shown to Claude. |

**Structured JSON output** (for `PreToolUse` blocking with a reason):
```json
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "deny",
    "permissionDecisionReason": "Use rg instead of grep for consistency"
  }
}
```

## Common stdin Fields (all events)

```json
{
  "session_id": "abc123",
  "cwd": "/path/to/project",
  "hook_event_name": "PreToolUse"
}
```

---

## Events

### SessionStart

**Fires:** When a session begins, resumes, or restarts after compaction.

**Matcher field:** `source` — values: `startup`, `resume`, `clear`, `compact`

**stdin:**
```json
{
  "session_id": "...",
  "cwd": "...",
  "hook_event_name": "SessionStart",
  "source": "compact"
}
```

**Common uses:**
- Re-inject context after compaction (`matcher: "compact"`)
- Load environment variables on startup (`matcher: "startup"`)
- Print project conventions reminder

---

### UserPromptSubmit

**Fires:** When the user submits a prompt, before Claude processes it.

**Matcher field:** *(none — fires on all prompts)*

**stdin:**
```json
{
  "session_id": "...",
  "cwd": "...",
  "hook_event_name": "UserPromptSubmit",
  "prompt": "the user's message text"
}
```

**Common uses:**
- Validate or sanitize user input
- Block prompts matching a pattern (exit 2)
- Inject additional context alongside every prompt

---

### PreToolUse

**Fires:** Before a tool executes. Can block execution (exit 2).

**Matcher field:** `tool_name` — e.g., `Bash`, `Edit`, `Write`, `Edit|Write`, `mcp__github__.*`

**stdin:**
```json
{
  "session_id": "...",
  "cwd": "...",
  "hook_event_name": "PreToolUse",
  "tool_name": "Edit",
  "tool_input": {
    "file_path": "/path/to/file",
    "old_string": "...",
    "new_string": "..."
  }
}
```

For `Bash` tools, `tool_input` contains `{"command": "the shell command"}`.

**Common uses:**
- Protect sensitive files from edits (exit 2 if path matches `.env`, secrets, etc.)
- Block dangerous shell commands (rm -rf, force push)
- Auto-approve safe permission patterns

---

### PostToolUse

**Fires:** After a tool completes successfully.

**Matcher field:** `tool_name` — same values as PreToolUse

**stdin:**
```json
{
  "session_id": "...",
  "cwd": "...",
  "hook_event_name": "PostToolUse",
  "tool_name": "Edit",
  "tool_input": {
    "file_path": "/path/to/file"
  },
  "tool_response": "the tool's output or result"
}
```

**Common uses:**
- Auto-format files after edits (`matcher: "Edit|Write"`)
- Log all file changes for audit
- Trigger a build or lint check

---

### PostToolUseFailure

**Fires:** After a tool fails.

**Matcher field:** `tool_name`

**stdin:** Same as PostToolUse plus `"error": "error message"`.

**Common uses:**
- Custom error recovery or logging
- Notify on repeated failures

---

### Notification

**Fires:** When Claude needs user attention (permission prompt, idle waiting).

**Matcher field:** `type` — values: `permission_prompt`, `idle_prompt`

**stdin:**
```json
{
  "session_id": "...",
  "cwd": "...",
  "hook_event_name": "Notification",
  "type": "permission_prompt",
  "message": "Claude is waiting for your input"
}
```

**Common uses:**
- Desktop notification so you know Claude needs you

macOS: `osascript -e 'display notification "Claude needs input" with title "Claude Code"'`
Linux: `notify-send 'Claude Code' 'Claude needs input'`

---

### Stop

**Fires:** When Claude finishes responding.

**Matcher field:** *(none)*

**stdin:**
```json
{
  "session_id": "...",
  "cwd": "...",
  "hook_event_name": "Stop",
  "stop_hook_active": false
}
```

**IMPORTANT — Infinite loop guard:** If a `Stop` hook outputs context that causes Claude to continue working, check `stop_hook_active` to prevent loops:
```bash
INPUT=$(cat)
if [ "$(echo "$INPUT" | jq -r '.stop_hook_active')" = "true" ]; then
  exit 0
fi
```

**Common uses:**
- Notify when Claude finishes a long task
- Run a verification agent to check task completion
- Trigger downstream processes

---

### PermissionRequest

**Fires:** When Claude requests permission for a tool call.

**Matcher field:** `tool_name`

**stdin:**
```json
{
  "session_id": "...",
  "cwd": "...",
  "hook_event_name": "PermissionRequest",
  "tool_name": "Bash",
  "tool_input": {"command": "npm install"}
}
```

**Output for auto-approval:**
```json
{
  "hookSpecificOutput": {
    "hookEventName": "PermissionRequest",
    "permissionDecision": "allow",
    "permissionDecisionReason": "npm install is always safe"
  }
}
```

**permissionDecision values:** `allow`, `deny`, `ask` (default/fallback)

---

### SubagentStart / SubagentStop

**Fires:** When a subagent spawns or exits.

**Matcher field:** *(none)*

**Common uses:** Monitor or constrain subagent behavior, log subagent activity.

---

### PreCompact

**Fires:** Before context compaction occurs.

**Matcher field:** *(none)*

**Common uses:** Save critical state to a file before the context window is trimmed.

---

### SessionEnd

**Fires:** When the session terminates.

**Matcher field:** `reason` — values: `clear`, `logout`, `other`

**Common uses:** Clean up temporary files, save session summary.

---

## Hook Script Best Practices

### Parse JSON input with jq
```bash
INPUT=$(cat)
FILE=$(echo "$INPUT" | jq -r '.tool_input.file_path // empty')
COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty')
```

### Block with a message
```bash
echo "Reason for blocking" >&2
exit 2
```

### Guard against shell profile echo pollution
If your `~/.zshrc` or `~/.bashrc` has unconditional `echo` statements, wrap them:
```bash
if [[ $- == *i* ]]; then
  echo "Shell ready"   # only in interactive shells
fi
```
Hooks run in non-interactive shells — unconditional echo corrupts JSON output.

### Test hooks manually
```bash
echo '{"tool_name":"Bash","tool_input":{"command":"rm -rf /"}}' | ./my-hook.sh
echo "Exit code: $?"
```

### Script file location
Place multi-line hook scripts in `.claude/hooks/` or a `scripts/` directory near your settings, then reference by path. Remember `chmod +x`.
