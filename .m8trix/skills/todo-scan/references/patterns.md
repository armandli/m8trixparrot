# Marker patterns

## Markers

`TODO`, `FIXME`, `XXX`, `HACK`, `BUG`. Case-sensitive (upper-case only) — this
avoids matching prose like "the redo stack".

## Regex

```python
import re

MARKER_RE = re.compile(
    r'(?:^|[^A-Za-z0-9_])'          # not preceded by an identifier char
    r'(TODO|FIXME|XXX|HACK|BUG)'    # group 1: the marker
    r'(?:\(([^)]{1,40})\))?'        # group 2: optional owner, e.g. TODO(alice)
    r'\s*[:\-]?\s*'                 # optional separator
    r'(.*)$'                        # group 3: the rest of the line
)
```

## Comment check

Only count a marker that appears after a comment opener on the same line. Cheap
heuristic that works for this repo's languages (C/C++, Python, CMake, shell,
Markdown, HTML):

```python
COMMENT_RE = re.compile(r'(//|#|--|/\*|\*|<!--)')

def in_comment(line: str, marker_start: int) -> bool:
    m = COMMENT_RE.search(line)
    return m is not None and m.start() < marker_start
```

Skip the line if `in_comment` is false — that filters out string literals and
identifiers such as `BUGGY_CONSTANT`.

## Owner

Group 2 of `MARKER_RE`. Print it in parentheses right after the marker; omit the
parentheses entirely when it is absent.
