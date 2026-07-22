---
name: skill-stat
description: Records skill usage statistics and issue reports into .claude/skill-stats.md. Increments the Uses count for a skill name, and optionally logs an issue report that increments the Issues count and appends a row to the Issue Reports table. Use when tracking how often a skill is invoked, when a user reports a problem with a skill, or when another skill needs to log its own usage. Trigger phrases: "record skill stat", "log skill usage", "report skill issue". Do NOT use for reading or displaying statistics — only for writing them.
argument-hint: "<skill-name> [issue report text]"
allowed-tools: Bash
---

Record skill usage (and optionally an issue) into `${PWD}/.claude/skill-stats.md`.

## Parameters

- `$1` — skill name (required). The name of the skill being recorded.
- `$2` — issue report string (optional). If provided, increments the Issues count AND appends a row to the Issue Reports table.

## Steps

### Step 1 — Validate Arguments

If `$1` is empty or not provided, stop and print:

```
Error: skill name is required. Usage: /skill-stat <skill-name> [issue-report]
```

### Step 2 — Run the Recording Script

If `$2` is provided (issue report present):

```bash
python3 ${PWD}/.claude/skills/skill-stat/scripts/record-stat.py "$1" "$2"
```

If `$2` is not provided:

```bash
python3 ${PWD}/.claude/skills/skill-stat/scripts/record-stat.py "$1"
```

### Step 3 — Report Result

Display the script's output to the user. If the script exits non-zero, show the error message and stop.
