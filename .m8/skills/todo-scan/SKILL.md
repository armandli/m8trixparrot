---
name: todo-scan
description: Find and triage TODO / FIXME / XXX / HACK / BUG comments across the repository, grouped and sorted by file. Use when the user asks to list, find, audit, or triage TODOs, tech-debt markers, or unfinished work in the codebase.
metadata:
  command: "true"
  argument-hint: "[path]   (a file or directory; defaults to the repo root)"
---

# todo-scan

Produce a report of unfinished-work markers in the codebase.

## Steps

1. Decide the search root: the argument if one was given, otherwise the current
   working directory.
2. With `python`, walk that tree. Skip `.git/`, `build/`, `env/`,
   `vcpkg_installed/`, and any path a `.gitignore` would exclude. Only read
   text files (skip anything with a NUL byte in the first 4 KB).
3. Match each line against the marker patterns. The canonical set — and the
   exact regex, including how to capture an owner in `TODO(alice):` — is in
   `references/patterns.md`; read it with `python` before writing the matcher.
4. For every hit record: file path (relative to the search root), line number,
   marker, owner (if any), and the trimmed comment text.

## Output

Group by file, files sorted alphabetically, hits within a file sorted by line
number. For each file print the path as a heading, then one line per hit:

```
src/core/agent.cpp
  L212  TODO         wire enable_subagents to a CLI flag
  L307  FIXME(walrus) system_prompt rebuilt every step is wasteful
```

End with a one-line summary: total hits, total files, and a count per marker.
If there are no hits, say so plainly.

## Notes

- Report only; never edit the files.
- A marker only counts inside a comment (after `//`, `#`, `--`, `/*`, `<!--`,
  or `*` in a block comment). A bare `TODO` in a string literal or identifier
  does not count — `references/patterns.md` has the check for this.
