---
name: create-hook
description: Interactive wizard that creates and installs Claude Code hooks in settings.json. Guides through lifecycle event selection, hook type (command/prompt/agent/http), matcher, and settings target, then writes the configuration. Use when user asks to "create a hook", "add a hook", "install a hook", "set up a hook for X", "make a hook that does Y", or "configure a hook". Do NOT use for general hook documentation or explaining what hooks are.
argument-hint: "[description of what the hook should do]"
---

## Overview

You are a hook creation wizard. Conduct a short interview to understand what the user wants, then generate and install the hook configuration into the appropriate settings file.

Work through the steps in order. Do not skip steps. Ask each question, wait for the user's answer, then proceed.

---

## Step 1 — Understand the Desired Behavior

If `$ARGUMENTS` is non-empty, treat it as the initial description and move to Step 2.

Otherwise, ask:

> "What do you want this hook to do? Describe the behavior in plain English (e.g., 'run prettier after every file edit', 'block writes to .env', 'notify me when Claude finishes')."

---

## Step 2 — Map to a Lifecycle Event

Using the description, recommend the best lifecycle event from the table below. Present your recommendation with one sentence of reasoning. If two events are plausible, offer both and ask the user to choose.

| Desired behavior | Event | Matcher hint |
|---|---|---|
| Run something after file edits | `PostToolUse` | `Edit\|Write` |
| Block dangerous shell commands | `PreToolUse` | `Bash` |
| Protect specific files from edits | `PreToolUse` | `Edit\|Write` |
| Validate before any tool runs | `PreToolUse` | *(none)* |
| Log or react after a tool succeeds | `PostToolUse` | *(tool name)* |
| Notify when Claude needs attention | `Notification` | *(none)* |
| Notify / verify when Claude stops | `Stop` | *(none)* |
| Re-inject context after compaction | `SessionStart` | `compact` |
| Run setup at session start | `SessionStart` | `startup` |
| Validate or transform user input | `UserPromptSubmit` | *(none)* |
| Auto-approve known-safe permissions | `PermissionRequest` | *(tool name)* |
| Save state before compaction | `PreCompact` | *(none)* |
| Clean up when session ends | `SessionEnd` | *(none)* |

For the full event list and stdin schemas, see [references/events-reference.md](references/events-reference.md).

---

## Step 3 — Choose Hook Type

Present the four options and recommend the best fit:

| Type | When to use |
|---|---|
| `command` | **Default.** Shell command for formatting, notifications, logging, file protection. |
| `prompt` | The decision requires understanding intent — a Claude model answers yes/no. |
| `agent` | Verification requires reading files or running tools — a subagent decides. |
| `http` | POST event data to an external service or webhook. |

Recommend `command` unless the user explicitly needs LLM judgment (`prompt`) or codebase inspection (`agent`).

---

## Step 4 — Configure the Hook Body

### command hooks

Ask: "What shell command should run?" If they describe behavior instead of a command, offer to generate one. Common patterns:

- **Auto-format a file**: `jq -r '.tool_input.file_path // empty' | xargs -I{} npx prettier --write "{}"`
- **Desktop notification (macOS)**: `osascript -e 'display notification "Claude needs input" with title "Claude Code"'`
- **Desktop notification (Linux)**: `notify-send 'Claude Code' 'Claude needs input'`
- **Protect a file** (exit 2 to block): a script that checks the path and exits 2 if it matches

If the command is multi-line or complex, suggest creating a script at `.claude/hooks/<name>.sh` and referencing it by path. Remind the user to run `chmod +x .claude/hooks/<name>.sh`.

For blocking hooks (`PreToolUse`), confirm: exit 2 + stderr message blocks the action. Offer to wrap the command in a guard:
```bash
#!/usr/bin/env bash
INPUT=$(cat)
FILE=$(echo "$INPUT" | jq -r '.tool_input.file_path // empty')
if [[ "$FILE" == *".env"* ]]; then
  echo "Blocked: .env files are protected" >&2
  exit 2
fi
```

### prompt hooks

Ask for the yes/no decision prompt text. Remind: yes = allow, no = block.

### agent hooks

Ask for the agent prompt. Suggest a timeout (default 60s). Remind: the agent has tool access and returns a decision.

### http hooks

Ask for the endpoint URL. Ask if authentication headers are needed (note: headers are configured in the hook object).

---

## Step 5 — Configure a Matcher (optional)

Ask: "Should this hook fire only for specific tools or sources, or for every occurrence of the event?"

If they want filtering, help construct the regex:

- Specific tools: `Edit|Write`, `Bash`, `mcp__github__.*`
- Session source: `startup`, `resume`, `compact`, `clear`
- Notification type: `permission_prompt`, `idle_prompt`

An empty or omitted matcher fires on every occurrence of the event. Omit if the user wants global coverage.

---

## Step 6 — Choose Installation Target

Ask where to install the hook:

1. **Global** — `~/.claude/settings.json` — applies to all projects on this machine
2. **Project** — `.claude/settings.json` — committed with the repo, shared with the team
3. **Project-local** — `.claude/settings.local.json` — project-specific, gitignored

Recommend:
- Personal automation (notifications, formatting preference) → **Global**
- Team enforcement (file protection, linting) → **Project**
- Machine-specific overrides → **Project-local**

---

## Step 7 — Install the Hook

1. Resolve the full path to the target settings file:
   - Global: `~/.claude/settings.json`
   - Project: `<cwd>/.claude/settings.json`
   - Project-local: `<cwd>/.claude/settings.local.json`

2. If the parent directory does not exist, create it.

3. Read the existing settings file if it exists. If it does not exist, start with `{}`.

4. Merge the new hook into the JSON structure:
   - Navigate to `hooks.<EventName>` (create the key if absent)
   - Build the hook entry:
     ```json
     {
       "matcher": "optional_regex",
       "hooks": [
         {
           "type": "command",
           "command": "your command here"
         }
       ]
     }
     ```
   - If `matcher` is empty, omit the `"matcher"` key entirely
   - Append this entry to the `hooks.<EventName>` array

5. Write the updated JSON back to the settings file (pretty-printed, 2-space indent).

**Error handling:**
- Invalid JSON in settings file: show the parse error, ask if the user wants to view and repair it before continuing
- Missing `.claude/` directory: create it with `mkdir -p`

---

## Step 8 — Verify and Report

1. Read the settings file back to confirm the hook was written correctly.
2. Report a summary:

```
Hook installed:
  Event:    <EventName>
  Type:     <command|prompt|agent|http>
  Matcher:  <regex or "(none — fires on all occurrences)">
  Location: <full path to settings file>

<If command type and script file referenced>
  Script:   <path>
  Action needed: run `chmod +x <path>` to make it executable

<If dependencies needed>
  Dependencies: <e.g., jq, prettier, osascript>
```

3. If the event is `PostToolUse` or `PreToolUse`, note that the hook will fire on the next tool invocation — no restart required.

---

## Final Step — Record Usage

After the skill's primary task completes, run:

```bash
python3 ${PWD}/.claude/skills/skill-stat/scripts/record-stat.py "create-hook"
```
