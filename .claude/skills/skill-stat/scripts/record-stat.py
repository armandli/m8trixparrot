#!/usr/bin/env python3
"""
record-stat.py — records skill usage statistics into .claude/skill-stats.md

Usage:
    python3 record-stat.py <skill-name>
    python3 record-stat.py <skill-name> <issue-report>

File format maintained:

    ## Usage Stats

    | Skill Name | Uses | Issues |
    |---|---|---|
    | my-skill | 3 | 1 |

    ## Issue Reports

    | Skill Name | Issue |
    |---|---|
    | my-skill | Something went wrong |
"""

import sys
import os
import fcntl

USAGE_TABLE_HDR = "| Skill Name | Uses | Issues |"
USAGE_TABLE_SEP = "|---|---|---|"
ISSUE_TABLE_HDR = "| Skill Name | Issue |"
ISSUE_TABLE_SEP = "|---|---|"


def parse_usage_table(lines):
    rows = {}
    for i, line in enumerate(lines):
        if line.strip() == USAGE_TABLE_HDR:
            j = i + 2  # skip separator line
            while j < len(lines):
                stripped = lines[j].strip()
                if not stripped.startswith("|"):
                    break
                parts = [p.strip() for p in stripped.split("|")]
                if len(parts) >= 4:
                    name = parts[1]
                    try:
                        uses = int(parts[2])
                        issues = int(parts[3])
                    except ValueError:
                        uses, issues = 0, 0
                    rows[name] = [uses, issues]
                j += 1
            break
    return rows


def parse_issue_rows(lines):
    rows = []
    in_table = False
    for line in lines:
        if line.strip() == ISSUE_TABLE_HDR:
            in_table = True
            continue
        if in_table:
            stripped = line.strip()
            if stripped == ISSUE_TABLE_SEP:
                continue
            if stripped.startswith("|"):
                rows.append(stripped)
            else:
                break
    return rows


def build_stats_file(usage_rows, issue_rows):
    parts = []
    parts.append("## Usage Stats")
    parts.append("")
    parts.append(USAGE_TABLE_HDR)
    parts.append(USAGE_TABLE_SEP)
    for name in sorted(usage_rows):
        uses, issues = usage_rows[name]
        parts.append(f"| {name} | {uses} | {issues} |")
    parts.append("")

    if issue_rows:
        parts.append("## Issue Reports")
        parts.append("")
        parts.append(ISSUE_TABLE_HDR)
        parts.append(ISSUE_TABLE_SEP)
        for row in issue_rows:
            parts.append(row)
        parts.append("")

    return "\n".join(parts) + "\n"


def main():
    if len(sys.argv) < 2 or not sys.argv[1].strip():
        print("Error: skill name is required.", file=sys.stderr)
        print("Usage: record-stat.py <skill-name> [issue-report]", file=sys.stderr)
        sys.exit(1)

    skill_name = sys.argv[1].strip().replace("|", "-")
    issue_report = (
        sys.argv[2].strip().replace("|", "-")
        if len(sys.argv) > 2 and sys.argv[2].strip()
        else None
    )

    cwd = os.getcwd()
    stats_file = os.path.join(cwd, ".claude", "skill-stats.md")
    lock_file = os.path.join(cwd, ".claude", "skill-stats.lock")

    os.makedirs(os.path.dirname(stats_file), exist_ok=True)

    with open(lock_file, "w") as lock_fd:
        fcntl.flock(lock_fd.fileno(), fcntl.LOCK_EX)
        try:
            if os.path.exists(stats_file):
                with open(stats_file, "r") as f:
                    lines = f.read().splitlines()
            else:
                lines = []

            usage_rows = parse_usage_table(lines)
            issue_rows = parse_issue_rows(lines)

            if skill_name not in usage_rows:
                usage_rows[skill_name] = [0, 0]

            usage_rows[skill_name][0] += 1

            if issue_report:
                usage_rows[skill_name][1] += 1
                safe_issue = issue_report.replace("\n", " ")
                issue_rows.append(f"| {skill_name} | {safe_issue} |")

            new_content = build_stats_file(usage_rows, issue_rows)
            tmp_file = stats_file + ".tmp"
            with open(tmp_file, "w") as f:
                f.write(new_content)
            os.replace(tmp_file, stats_file)

        finally:
            fcntl.flock(lock_fd.fileno(), fcntl.LOCK_UN)

    if issue_report:
        print(f"Recorded: {skill_name} — uses incremented, issue logged.")
    else:
        print(f"Recorded: {skill_name} — uses incremented.")


if __name__ == "__main__":
    main()
