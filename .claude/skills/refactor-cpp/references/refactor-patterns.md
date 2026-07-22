# Refactor Patterns Reference

Each section covers one **operand type** and lists the signals to look for,
template decision, similarity thresholds, Auto/Suggest conditions, the target
utility files, and before/after C++ examples.

---

## R01 — String Operations

**Utility files:** `utils/string_utils.h` + `utils/string_utils.cpp`
**Template?** No — operates on `std::string` / `std::string_view` only

### Signals
- Same prefix or suffix stripped in ≥ 2 places
  (e.g., `s.substr(prefix.size())` or `s.starts_with(prefix)` followed by
  `s.substr(...)`)
- Identical `std::regex` pattern constructed and applied in ≥ 2 functions
- Same format-and-pad pattern: right/left-align with `std::setw` in ≥ 2 places
- Same sanitization pipeline: trim whitespace → lowercase → replace applied in
  sequence in ≥ 2 files
- Same truncation-with-ellipsis pattern
  (e.g., `s.size() > n ? s.substr(0, n) + "..." : s`) in ≥ 2 places

### Template Decision
Non-template. All operations are over `std::string_view` (read-only input) or
`std::string` (owned output). No type parameterization needed.

### Similarity Threshold
| Level | Description |
|-------|-------------|
| Exact | Identical code block, same string literal or length constant |
| Parameterized | Same operation, different string constant or length limit |
| Semantic | Same result via different C++ idioms — **Suggest only** |

### Auto Conditions
- Same prefix or suffix literal stripped in ≥ 2 locations → extract with a
  `std::string_view prefix` parameter
- Same `std::regex` pattern string constructed in ≥ 2 locations → extract with
  a static compiled-regex helper
- Same truncation length and sentinel in ≥ 3 locations

### Suggest Conditions
- Two locations only, or the regex pattern differs by small amounts (may have
  diverged intentionally)
- Sanitization pipeline where one call site has extra steps (asymmetric
  pipelines)
- Any string operation that depends on locale

### Before / After Example

```cpp
// Before (in multiple .cpp files)
std::string a = raw.substr(5);  // strip "user_" prefix (length 5)
std::string b = text.substr(5);

// After — utils/string_utils.h
#ifndef UTILS_STRING_UTILS_H
#define UTILS_STRING_UTILS_H

#include <string>
#include <string_view>

namespace utils {

// Remove prefix from s if present; return s unchanged otherwise.
std::string strip_prefix(std::string_view s, std::string_view prefix);

}  // namespace utils

#endif  // UTILS_STRING_UTILS_H

// After — utils/string_utils.cpp
#include "utils/string_utils.h"

namespace utils {

std::string strip_prefix(std::string_view s, std::string_view prefix) {
  if (s.starts_with(prefix)) {
    return std::string(s.substr(prefix.size()));
  }
  return std::string(s);
}

}  // namespace utils

// Updated call sites
#include "utils/string_utils.h"

std::string a = utils::strip_prefix(raw, "user_");
std::string b = utils::strip_prefix(text, "user_");
```

---

## R02 — Numeric / Mathematical Calculations

**Utility files:** `utils/numeric_utils.h` (header-only when template; add
`utils/numeric_utils.cpp` only for non-template overloads)
**Template?** Yes when the formula must work over multiple arithmetic types
(e.g., `int`, `float`, `double`); No when a single concrete type suffices

### Signals
- Same formula in ≥ 2 functions (e.g., compound interest:
  `principal * std::pow(1.0 + rate, periods)`)
- Same unit conversion (km ↔ mi, °C ↔ °F) applied in ≥ 2 places
- Same rounding or clamping applied with the same bounds in ≥ 2 call sites
  that share the same business meaning
- Same normalization / min-max scaling:
  `(x - min_val) / (max_val - min_val)` in ≥ 2 files
- Identical arithmetic expression with the same named constants in ≥ 2 files

### Template Decision
Template when:
- The formula must operate on `int`, `float`, `double`, or user-defined
  arithmetic types (use `template <typename T>` or a C++20 concept)
- The concrete type varies between call sites

Non-template when:
- All call sites use exactly one concrete type (e.g., all `double`)

### Similarity Threshold
| Level | Description |
|-------|-------------|
| Exact | Identical formula, identical coefficient literals |
| Parameterized | Same formula, varying rate/period/coefficient (extract as parameter) |
| Semantic | Algebraically equivalent but written differently — **Suggest only** |

### Auto Conditions
- Exact or parameterized formula in ≥ 2 locations, no side effects, pure
  function (inputs → output only)
- Unit conversion: same conversion factor used in ≥ 2 places

### Suggest Conditions
- Algebraically equivalent expressions (verify manually before merging)
- Formulas inside `try/catch` blocks where exception handling differs
- Formulas that write to external state or call non-pure functions

### Before / After Example

```cpp
// Before (in multiple .cpp files, both use double)
double monthly = principal * rate / (1.0 - std::pow(1.0 + rate, -n));
double cost    = loan * interest / (1.0 - std::pow(1.0 + interest, -terms));

// After — utils/numeric_utils.h (template, header-only)
#ifndef UTILS_NUMERIC_UTILS_H
#define UTILS_NUMERIC_UTILS_H

#include <cmath>

namespace utils {

// Return the fixed periodic payment for an amortized loan.
template <typename T>
T amortized_payment(T principal, T rate, int n_payments) noexcept {
  return principal * rate / (T{1} - std::pow(T{1} + rate, -n_payments));
}

}  // namespace utils

#endif  // UTILS_NUMERIC_UTILS_H

// Updated call sites
#include "utils/numeric_utils.h"

double monthly = utils::amortized_payment(principal, rate, n);
double cost    = utils::amortized_payment(loan, interest, terms);
```

---

## R03 — Container Operations

**Utility files:** `utils/container_utils.h` (header-only)
**Template?** Almost always yes — container utilities must be generic over
element type and container type

### Signals
- Same filter predicate applied to different containers in ≥ 2 places
  (e.g., iterating and `push_back` where `elem.active && elem.role == "admin"`)
- Same sort comparator expression repeated in ≥ 2 places
- Same deduplication pattern: sort + `std::unique` + erase in ≥ 2 places
- Same partition-by-predicate pattern (splitting a range into two halves)
- Same linear search returning an iterator or index in ≥ 2 files

### Template Decision
Always template. Containers and element types vary at each call site.

### Similarity Threshold
| Level | Description |
|-------|-------------|
| Exact | Identical predicate including field names and literal values |
| Parameterized | Same structure, differing threshold or attribute (extract predicate as parameter) |
| Semantic | Equivalent result via different STL idioms — **Suggest only** |

### Auto Conditions
- Identical predicate (same fields and literals) in ≥ 2 locations operating on
  the same element type
- Identical sort comparator in ≥ 2 locations
- Identical deduplication sequence (exact code match) in ≥ 2 locations

### Suggest Conditions
- Predicates that close over outer-scope mutable state
- Filter + transform combined in one pass (extraction may change intermediate
  representation)
- Range operations where iterator invalidation rules differ between call sites

### Before / After Example

```cpp
// Before (in multiple .cpp files)
std::vector<User> admins;
for (const auto& u : users) {
  if (u.active and u.role == "admin") admins.push_back(u);
}

std::vector<Member> admin_members;
for (const auto& m : members) {
  if (m.active and m.role == "admin") admin_members.push_back(m);
}

// After — utils/container_utils.h (template, header-only)
#ifndef UTILS_CONTAINER_UTILS_H
#define UTILS_CONTAINER_UTILS_H

#include <vector>

namespace utils {

// Return elements from range [first, last) satisfying pred.
template <typename InputIt, typename Pred>
auto filter(InputIt first, InputIt last, Pred pred) {
  using T = typename std::iterator_traits<InputIt>::value_type;
  std::vector<T> result;
  for (auto it = first; it != last; ++it) {
    if (pred(*it)) result.push_back(*it);
  }
  return result;
}

}  // namespace utils

#endif  // UTILS_CONTAINER_UTILS_H

// Updated call sites
#include "utils/container_utils.h"

auto admins = utils::filter(
    users.begin(), users.end(),
    [](const auto& u) { return u.active and u.role == "admin"; });

auto admin_members = utils::filter(
    members.begin(), members.end(),
    [](const auto& m) { return m.active and m.role == "admin"; });
```

---

## R04 — Validation / Guard

**Utility files:** `utils/validation_utils.h` + `utils/validation_utils.cpp`
for non-template guards; range-check templates stay in `.h` only
**Template?** Yes for range checks (must work over numeric types); No for
null/empty/format checks (concrete types only)

### Signals
- Same guard pattern repeated:
  `if (s.empty()) throw std::invalid_argument("...");` in ≥ 2 places
- Same range check: `if (value < MIN or value > MAX) throw ...` with same
  `MIN`/`MAX` in ≥ 2 places
- Same regex validation (email, phone, postal code) applied in ≥ 2 functions
- Same null-pointer check + error in ≥ 2 places
- Same combined type + range check in ≥ 2 files

### Template Decision
Template when:
- The range check must work over `int`, `long`, `double`, etc. (use
  `template <typename T>`)

Non-template when:
- The check operates on one concrete type (`std::string_view`, `void*`, etc.)

### Similarity Threshold
| Level | Description |
|-------|-------------|
| Exact | Identical guard expression and identical error message |
| Parameterized | Same guard shape, different threshold or field name (extract as parameter) |
| Semantic | Equivalent but raises different exception types — **Suggest only** |

### Auto Conditions
- Identical guard pattern (same predicate, same exception class, same message)
  in ≥ 2 locations
- Same regex pattern string in ≥ 2 locations for the same data type

### Suggest Conditions
- Same guard expression but different exception types or messages across sites
  (semantics may diverge intentionally)
- Guards that also perform logging or metric emission alongside throwing
- Any validation inside a `try/catch` where the caller depends on the exact
  exception type

### Before / After Example

```cpp
// Before — empty-string check (non-template, appears in multiple .cpp files)
if (name.empty()) throw std::invalid_argument("name must not be empty");
if (email.empty()) throw std::invalid_argument("email must not be empty");

// After — utils/validation_utils.h
#ifndef UTILS_VALIDATION_UTILS_H
#define UTILS_VALIDATION_UTILS_H

#include <stdexcept>
#include <string_view>

namespace utils {

// Throw std::invalid_argument if s is empty.
void throw_if_empty(std::string_view s, std::string_view field_name);

// Throw std::out_of_range if value is outside [lo, hi].
template <typename T>
void throw_if_out_of_range(T value, T lo, T hi, std::string_view field_name) {
  if (value < lo or value > hi) {
    throw std::out_of_range(
        std::string(field_name) + " out of range");
  }
}

}  // namespace utils

#endif  // UTILS_VALIDATION_UTILS_H

// After — utils/validation_utils.cpp
#include "utils/validation_utils.h"

namespace utils {

void throw_if_empty(std::string_view s, std::string_view field_name) {
  if (s.empty()) {
    throw std::invalid_argument(
        std::string(field_name) + " must not be empty");
  }
}

}  // namespace utils

// Updated call sites
#include "utils/validation_utils.h"

utils::throw_if_empty(name, "name");
utils::throw_if_empty(email, "email");
```

---

## R05 — I/O / Serialization

**Utility files:** `utils/io_utils.h` + `utils/io_utils.cpp`
**Template?** No (usually) — file paths and serialization formats are
concrete; use `std::string_view` / `std::filesystem::path` for parameters

### Signals
- Same file-path construction pattern:
  `base_dir / category / (name + ".json")` repeated with the same `base_dir`
  in ≥ 2 places
- Same file read-all-bytes or read-line-by-line wrapper in ≥ 2 functions
- Same JSON key extraction chain with the same key sequence in ≥ 2 places
- Same `std::ofstream` / `std::ifstream` open-write-close pattern with
  identical flags in ≥ 2 files
- Same binary serialization: write a fixed-layout struct with `write()` in
  ≥ 2 places

### Template Decision
Non-template in most cases. If the same read/write logic must handle multiple
value types (e.g., reading numeric fields as `int` or `double`), a template
overload is acceptable — keep the full definition in the `.h`.

### Similarity Threshold
| Level | Description |
|-------|-------------|
| Exact | Identical path construction or key chain |
| Parameterized | Same structure, varying key name or base directory (extract as parameter) |
| Semantic | Equivalent result via different I/O libraries — **Suggest only** |

### Auto Conditions
- Identical file-path construction in ≥ 2 locations (same base directory, same
  join structure)
- Identical `std::ofstream` open + write + close sequence in ≥ 2 locations

### Suggest Conditions
- File I/O inside `try/catch` blocks where exception handling differs between
  sites
- Patterns that mix reading and writing state in the same block
- Serialization that depends on platform-specific byte order

### Before / After Example

```cpp
// Before (in multiple .cpp files)
#include <filesystem>
namespace fs = std::filesystem;

fs::path path_a = base_dir / "reports" / (report_id + ".json");
fs::path path_b = base_dir / "reports" / (doc_id + ".json");

// After — utils/io_utils.h
#ifndef UTILS_IO_UTILS_H
#define UTILS_IO_UTILS_H

#include <filesystem>
#include <string_view>

namespace utils {

// Return the canonical path for a report JSON file.
std::filesystem::path report_path(
    std::string_view report_id,
    const std::filesystem::path& base_dir);

}  // namespace utils

#endif  // UTILS_IO_UTILS_H

// After — utils/io_utils.cpp
#include "utils/io_utils.h"

namespace utils {

std::filesystem::path report_path(
    std::string_view report_id,
    const std::filesystem::path& base_dir)
{
  return base_dir / "reports" / (std::string(report_id) + ".json");
}

}  // namespace utils

// Updated call sites
#include "utils/io_utils.h"

auto path_a = utils::report_path(report_id, base_dir);
auto path_b = utils::report_path(doc_id, base_dir);
```

---

## R06 — Algorithm / Computation

**Utility files:** `utils/algorithm_utils.h` (header-only)
**Template?** Almost always yes — algorithmic utilities must be generic over
element type, comparator, and container shape

### Signals
- Same binary search variant applied to a sorted container in ≥ 2 places
- Same sliding-window or two-pointer loop structure in ≥ 2 files
- Same clamp / saturate pattern: `std::max(lo, std::min(hi, value))` in ≥ 2
  places (pre-C++17 projects only; C++17 has `std::clamp`)
- Same weighted-sum accumulation over a range in ≥ 2 functions
- Same running-average or running-max update in ≥ 2 files

### Template Decision
Always template. Algorithms are type-agnostic by nature.

### Similarity Threshold
| Level | Description |
|-------|-------------|
| Exact | Identical loop structure including predicate literals |
| Parameterized | Same algorithm, differing comparator or weight function (pass as parameter) |
| Semantic | Equivalent result via different STL algorithms — **Suggest only** |

### Auto Conditions
- Identical loop body (same structure, same predicate) in ≥ 2 locations
- Same clamp / saturate expression with the same bound variables in ≥ 3
  locations

### Suggest Conditions
- Algorithms with subtle invariant requirements that differ between call sites
- Loops that mutate an external data structure alongside computing a result
- Two locations only (prefer Suggest unless extraction is trivial)

### Before / After Example

```cpp
// Before (in multiple .cpp files, pre-C++17 clamp)
int clamped_age   = std::max(0, std::min(150, age));
double clamped_rate = std::max(0.0, std::min(1.0, rate));

// After — utils/algorithm_utils.h (template, header-only)
#ifndef UTILS_ALGORITHM_UTILS_H
#define UTILS_ALGORITHM_UTILS_H

#include <algorithm>

namespace utils {

// Return value clamped to [lo, hi].
template <typename T>
constexpr T clamp(T value, T lo, T hi) noexcept {
  return std::max(lo, std::min(hi, value));
}

}  // namespace utils

#endif  // UTILS_ALGORITHM_UTILS_H

// Updated call sites
#include "utils/algorithm_utils.h"

int clamped_age     = utils::clamp(age, 0, 150);
double clamped_rate = utils::clamp(rate, 0.0, 1.0);
```

---

## R07 — Domain-specific

**Utility files:** `utils/<domain>_utils.h` (+ `utils/<domain>_utils.cpp` for
non-template definitions)
**Template?** Yes if the formula is type-generic (e.g., a unit conversion that
must work over `float` and `double`); No for concrete domain formulas

This is a catch-all for business formulas that do not fit cleanly into the
categories above — tax brackets, scoring algorithms, fee calculations, penalty
schedules, rate tables, etc.

### Signals
- An identical arithmetic expression referencing named domain constants in ≥ 2
  files (e.g., same tax rate applied multiple times)
- Same scoring formula or weighted sum in ≥ 2 functions
- Same penalty or discount calculation referenced in multiple modules
- `inline constexpr` constants (or raw literals) defined redundantly in
  multiple files (e.g., `constexpr double TAX_RATE = 0.07;` in two `.h` files)

### Template Decision
Template when:
- The formula is type-generic (works over `float`, `double`, user-defined
  numeric types)

Non-template when:
- All call sites use the same concrete type and there is no benefit to
  generalizing

### Similarity Threshold
| Level | Description |
|-------|-------------|
| Exact | Identical expression and identical constant values |
| Parameterized | Same formula, varying bracket threshold or rate (extract as parameter) |
| Semantic | Economically equivalent but algorithmically different — **Suggest only** |

### Auto Conditions
- Exact formula in ≥ 2 files, pure function (no I/O, no state mutation)
- Same constant defined with the same value in ≥ 2 files → extract to the
  utility header as `inline constexpr` and import everywhere

### Suggest Conditions
- Formulas that may need to change independently in each module (intentional
  divergence)
- Tax or regulatory calculations where different sites use different rule sets
- Any domain formula inside exception-handling logic

### Before / After Example

```cpp
// Before (in billing.cpp and invoicing.cpp)
// billing.cpp
constexpr double TAX_RATE = 0.07;
double total = subtotal * (1.0 + TAX_RATE);

// invoicing.cpp
constexpr double TAX_RATE = 0.07;
double invoice_total = net * (1.0 + TAX_RATE);

// After — utils/tax_utils.h
#ifndef UTILS_TAX_UTILS_H
#define UTILS_TAX_UTILS_H

namespace utils {

inline constexpr double TAX_RATE = 0.07;

// Return amount with tax applied at the given rate.
double apply_tax(double amount, double rate = TAX_RATE) noexcept;

}  // namespace utils

#endif  // UTILS_TAX_UTILS_H

// After — utils/tax_utils.cpp
#include "utils/tax_utils.h"

namespace utils {

double apply_tax(double amount, double rate) noexcept {
  return amount * (1.0 + rate);
}

}  // namespace utils

// Updated call sites (billing.cpp)
#include "utils/tax_utils.h"

double total = utils::apply_tax(subtotal);

// Updated call sites (invoicing.cpp)
#include "utils/tax_utils.h"

double invoice_total = utils::apply_tax(net);
```

---

## Summary Tables

### Template vs Non-template Decision Matrix

| ID | Category | Template? | Reason |
|----|----------|-----------|--------|
| R01 | String Operations | No | Operates on `std::string_view` / `std::string` only |
| R02 | Numeric / Mathematical | Yes if multi-type | Formula may need `int`, `float`, `double` |
| R03 | Container Operations | Almost always yes | Must be generic over element and container type |
| R04 | Validation / Guard | Yes for range checks; No for null/empty | Range checks are type-generic; format checks are concrete |
| R05 | I/O / Serialization | No (usually) | File paths and formats are concrete |
| R06 | Algorithm / Computation | Almost always yes | Algorithms are type-agnostic by nature |
| R07 | Domain-specific | Yes if type-generic formula | Non-template when all call sites share one concrete type |

### File Placement Rules

| Function type | Header (`.h`) | Source (`.cpp`) |
|---------------|--------------|-----------------|
| Template function | Full definition (declaration + body) | Never — definition must be in `.h` |
| Non-template function | Declaration only | Definition |
| `inline constexpr` constant | Definition (use `inline constexpr`) | Never |
| `static` helper (file-internal) | Never | Definition in anonymous namespace |
