---
name: refactor-cpp
description: Scans a C++ codebase for duplicate or near-duplicate logic patterns across .cpp and .h files, then extracts them into reusable utility functions in a shared utils/ directory. Use when the user asks to "refactor this C++ code", "find duplicate logic in C++", "extract shared C++ utilities", "apply DRY to C++", "deduplicate C++ code", or "find repeated patterns in C++". Groups extracted helpers by the object type they operate on (strings, numbers, containers, etc.). Template functions are placed in .h headers only; non-template utilities get a .h declaration and a .cpp definition. Do NOT use for performance optimization, debugging logic errors, code formatting (use format-cpp), or explaining code. Do NOT extract code that appears only once. Run this skill before format-cpp.
argument-hint: "[file or directory path]"
---

## Workflow

### Step 1: Identify Target Files

If `$ARGUMENTS` is provided, use it as the target path. Otherwise, scan the
C++ code most recently written or discussed in the conversation.

- Directory → find all `.cpp`, `.cc`, `.cxx`, `.h`, `.hpp`, `.hxx` files recursively
- Single file → scan that file only
- **Skip always:** `build/`, `cmake-build-*/`, `third_party/`, `vendor/`,
  `external/`, `deps/`
- **Skip by default:** test files (`*_test.cpp`, `*_unittest.cpp`, `test/` or
  `tests/` directories) unless the user explicitly includes them
- Read every target file before making any modifications

### Step 2: Scan for Duplicate Logic

Read all target files and search for:

1. **Exact duplicates** — identical or near-identical code blocks (differing only
   in whitespace or comments) appearing ≥ 2 times across different functions,
   classes, or files
2. **Parameterized variants** — same logic differing only by a constant value,
   string literal, threshold, formula coefficient, or type name
3. **Semantic duplicates** — same computation or manipulation expressed with
   trivially different syntax (see
   [references/refactor-patterns.md](references/refactor-patterns.md) for
   per-category signals)

**Minimum threshold:**
- A pattern must appear in **≥ 2 distinct files** to qualify for extraction
- Prefer **≥ 3 sites** before Auto-classifying; with exactly 2, default to
  Suggest unless the duplication is exact and the extraction is trivial

Classify every finding as:

| Class | Meaning | Action |
|-------|---------|--------|
| **Auto** | Safe mechanical extraction; all occurrences are semantically equivalent | Extract and replace |
| **Suggest** | Needs judgment — structural change, uncertain equivalence, or side-effect concern | Report only, do not apply |

**Auto-extraction threshold:** only extract when you are certain every occurrence
has the same return type, the same side-effect profile, and would behave
identically when replaced by a call to the new utility. When in doubt, classify
as Suggest.

### Step 3: Classify and Group Findings

For each finding, assign:

- **Class:** Auto or Suggest (see thresholds above)
- **Operand type:** one of String, Numeric, Container, Validation, I/O,
  Algorithm, or Domain-specific (see
  [references/refactor-patterns.md](references/refactor-patterns.md))
- **Template decision:** determine whether the extracted function must be a
  function template (operates on multiple concrete types) or a plain function
  (single concrete type); templates go in `.h` only, plain functions get a `.h`
  declaration and a `.cpp` definition
- **Proposed function signature:** include `const`, `noexcept`, `&`, and `&&`
  qualifiers as appropriate; use `std::string_view` for read-only string
  parameters; prefer `const T&` for read-only input, `T&&` for sinks;
  prefer return type over output parameters — only use a non-`const` reference
  parameter when the caller must supply an existing object to be filled or
  when returning multiple values with no natural primary result; if output
  parameters are unavoidable, list them **before** all input parameters
- **Target utility file path:** `utils/<type>_utils.h` (and `.cpp` if
  non-template)

Group all findings by operand type to determine which utility files to create
or update.

### Step 4: Create Utility Files

For each operand type group that has **≥ 1 Auto finding:**

1. Determine output path: `<project_root>/utils/<type>_utils.h` (and `.cpp`
   for non-template definitions)
2. If the `utils/` directory does not exist, create it
3. If a utility file **already exists**, read it first, check for name
   collisions, then **append** new content — never overwrite
4. **Template functions** → define fully in the `.h` file; never split the
   definition into a `.cpp` file (the compiler requires the definition visible
   at every instantiation site)
5. **Non-template functions** → declare in `.h`, define in `.cpp`
6. Use `#ifndef` header guards (not `#pragma once`); guard name is
   `UTILS_<TYPE>_UTILS_H` in `UPPER_SNAKE_CASE`
7. Wrap all declarations in `namespace utils { }`
8. Use `inline constexpr` for constants shared across translation units
9. Do not place `using namespace` at file scope in any header

**One function per distinct logical operation.** Do not merge unrelated helpers
into a single function.

### Step 5: Replace Call Sites

For each Auto finding:

1. Add `#include "utils/<type>_utils.h"` at the top of every modified source
   file (place after the corresponding header in `.cpp` files, following
   include ordering conventions)
2. Qualify all calls with `utils::` (e.g., `utils::strip_prefix(s, "user_")`)
3. Preserve surrounding comments verbatim
4. Do not change any surrounding code beyond the immediate replacement
5. **Build system note:** if a new `utils/<type>_utils.cpp` file is created,
   report that it must be registered in `CMakeLists.txt`, `Makefile`, or
   `BUILD` — do not edit build files automatically

### Step 6: Report

After all files are processed, output a structured summary:

```
## Refactor Report

### Extracted (N utilities created/updated)
- utils/<file>.h — utils::<function>: extracted from <file1>:<line>, <file2>:<line>, ...

### Replaced (N call sites updated)
- <file>:<line> — replaced with utils::<function>(...)

### Suggested (N items — not applied)
- <file>:<line> — <description>: <why it qualifies> — Recommended: <proposed signature>

### Build System Note
- New .cpp file(s) requiring build system registration: <list>

### Skipped
- <any patterns considered but not extracted, and why>
```

Keep descriptions concise. For Suggest items, include a short code snippet
showing the recommended extracted form when it aids clarity.

---

## Safety Rules

- **Never alter existing public function signatures or break ABI** — only
  replace the internal body where the duplicate logic appears
- **Never remove code without replacing every call site first** — do not leave
  orphaned references
- **Template functions → `.h` ONLY** — never define a template function body
  in a `.cpp` file; the compiler requires the definition visible at every
  instantiation site
- **Non-template functions** → declare in `.h`, define in `.cpp`
- **Preserve `noexcept` correctness** — if uncertain whether the extracted
  function can guarantee no-throw, classify as Suggest rather than marking
  `noexcept`
- **No mutable statics or global state** — do not extract code that reads or
  writes mutable `static` local variables or global mutable state
- **Append, never overwrite** — if a utility file already exists, append new
  content; never truncate or replace existing content
- **No member-state extraction** — do not extract code that reads or writes
  member variables unless those values are passed as explicit parameters to
  the utility function
- **Prefer `const T&` / `std::string_view`** — use `const T&` for read-only
  inputs, `T&&` for sinks, and `std::string_view` for read-only string
  parameters (C++17 or later)
- **Prefer return value over output parameters** — return the result directly
  whenever possible; only introduce a non-`const` reference output parameter
  when the caller must supply an existing object to fill or when returning
  multiple values with no natural primary result; if output parameters are
  required, place them **before** all input parameters in the signature
- **No `using namespace` at file scope in headers** — headers must not pollute
  the global namespace
- **Semantic equivalence required for Auto** — only extract into Auto when all
  occurrences have the same return type and the same side-effect profile
- **One function per operation** — do not merge unrelated helpers into one
  function

---

## Additional Resources

- For per-category signals, template decisions, similarity thresholds, and
  before/after examples, see
  [references/refactor-patterns.md](references/refactor-patterns.md)

---

## Final Step — Record Usage

After the skill's primary task completes, run:

```bash
python3 ${PWD}/.claude/skills/skill-stat/scripts/record-stat.py "refactor-cpp"
```
