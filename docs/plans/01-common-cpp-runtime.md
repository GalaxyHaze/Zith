# Step 1 --- Common C++ Runtime Reorganization

## Summary

Reorganize `common/` with physical subdirectories matching sub-namespaces.
Relocate all current `memory::` types into `common/memory/` under
`namespace common::memory`. Introduce `common::diagnostic` and `common::text`
as new sub-namespaces with their own directories. `common/` root holds only
types genuinely at `namespace common` level (CMakeLists, README, and any
future top-level common types).

Remove local `Span` declarations from all `types.hpp` and use the canonical
`common::memory::Span` from `common/memory/span.hpp`.

## Directory Layout After Step 1

```
src/common/
  CMakeLists.txt
  README.md
  memory/
    arena.hpp / arena.cpp
    dyn-array.hpp
    flat-map.hpp
    optional.hpp
    result.hpp
    source-file.hpp / source-file.cpp
    source-map.hpp / source-map.cpp
    span.hpp
    string-interner.hpp / string-interner.cpp
  diagnostic/
    diagnostic.hpp
    levenshtein.cpp
  text/
    parse.hpp
    parse.cpp
```

## Namespace Mapping

| Directory | Namespace |
|-----------|-----------|
| `common/memory/` | `common::memory` |
| `common/diagnostic/` | `common::diagnostic` |
| `common/text/` | `common::text` |

Nothing currently lives at `common::` top-level. If a future type genuinely
belongs at `common::`, its header stays in `common/` root.

## Design Decisions

- `memory::` becomes `common::memory::`. All current `common/*.hpp`/`.cpp`
  files are physically moved into `common/memory/` and wrapped with
  `namespace common { namespace memory { ... } }` (or `namespace common::memory`).
- `common/memory/span.hpp`: `Span` removes its `FileId file` member. The canonical
  form is `{ByteOffset start, ByteOffset end}` with `len()`, `contains()`,
  `isValid()`, `merge()`. File-aware code uses `SourceSpan { FileId file; Span span; }`
  declared in `common/memory/source-map.hpp`.
- New sub-namespace `common::text` holds parsing primitives currently emitted
  as inline C++ strings by generators: `parseBool`, `parseInt`, `parseLong`,
  `parseStringList`, `parseString`.
- New sub-namespace `common::diagnostic` holds `Diagnostic`, `Severity`,
  `levenshteinDistance`, `bestSuggestion`, and `hasPlausiblePrefix`.

## Implementation Changes

### 1.1 --- Physical move into common/memory/

- Create `src/common/memory/`.
- Move these files from `src/common/` into `src/common/memory/`:
  `arena.hpp`, `arena.cpp`, `dyn-array.hpp`, `flat-map.hpp`, `optional.hpp`,
  `result.hpp`, `source-file.hpp`, `source-file.cpp`, `source-map.hpp`,
  `source-map.cpp`, `span.hpp`, `string-interner.hpp`, `string-interner.cpp`.
- In each moved header, replace `#pragma once` guard includes to use the new
  relative paths (e.g. `#include "common/memory/arena.hpp"` instead of
  `"common/arena.hpp"`).
- Wrap every moved file with `namespace common::memory { ... }` instead of
  bare `namespace memory { ... }`.
- `src/common/CMakeLists.txt`: update all source paths to `memory/arena.cpp`,
  `memory/source-file.cpp`, etc. Update `target_include_directories` to
  include `common/memory/` so `#include "arena.hpp"` still resolves
  within the memory subdirectory.
- Run a mechanical sed across `src/` and `tests/`:
  `s!\bmemory::!\common::memory::!g`.
  This covers generator Python string literals that emit `memory::`,
  all `.cpp`/`.hpp`, and CMakeLists.
- Update all generator `.py` files that write `memory::` in C++ string
  emissions.
- Update `common/README.md` namespace and directory documentation.

### 1.2 --- Canonical Span

- `common/memory/span.hpp`: remove `FileId file` member. Retain `start`, `end`,
  `len()`, `contains()`, `isValid()`, `merge()`.
- `common/memory/source-map.hpp`: add `struct SourceSpan { FileId file; Span span; };`.
  All `SourceMap` methods that currently take or return `Span` now use
  `SourceSpan` instead.
- `common/memory/source-map.cpp`: update method bodies for `SourceSpan`.
- Delete the local `Span` definitions from:
  `src/frontend/lexer/types.hpp:5`,
  `src/frontend/parser/types.hpp:8`,
  `src/frontend/ast/types.hpp:5`.
  Each now includes `"common/memory/span.hpp"` directly, or the type
  resolves transitively via the generated headers' include paths.
- `src/session/dispatch.cpp:17`: change `Diagnostic{.span = {.file = 0, ...}}`
  to use `SourceSpan` or the new two-field `Span`.

### 1.3 --- common::diagnostic

New files under `src/common/diagnostic/`:

- `diagnostic.hpp`:

```cpp
#pragma once
#include "common/memory/span.hpp"
#include <string>
#include <string_view>

namespace common::diagnostic {

enum class Severity { Note, Warning, Error };

struct Diagnostic {
    memory::Span span;
    Severity severity = Severity::Error;
    std::string message;
};

std::size_t levenshteinDistance(std::string_view a, std::string_view b);
bool hasPlausiblePrefix(const char *arg, const char *candidate);
const char *bestSuggestion(const char *arg, const char *const *candidates);

} // namespace common::diagnostic
```

- `levenshtein.cpp` --- implementations of the three functions above,
  extracted from the inline C++ currently emitted in
  `src/cli/generate.py:1293-1360`.
- `src/common/CMakeLists.txt`: add `diagnostic/levenshtein.cpp` to
  `_zith_common_sources`.
- CLI generator: replace the ~80 emitted lines of suggestion logic with
  `#include "common/diagnostic/diagnostic.hpp"` and calls to
  `common::diagnostic::levenshteinDistance` / `common::diagnostic::bestSuggestion`.
- Session generator: update `src/session/generate.py` to emit
  `common::diagnostic::Diagnostic` instead of its own `struct Diagnostic`.

### 1.4 --- common::text

New files under `src/common/text/`:

- `parse.hpp`:

```cpp
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace common::text {

bool parseBool(std::string_view s, bool &out) noexcept;
bool parseInt(std::string_view s, int &out) noexcept;
bool parseLong(const char *text, long &value) noexcept;
bool parseString(std::string_view value, std::string &out) noexcept;
bool parseStringList(std::string_view value, std::vector<std::string> &out) noexcept;

} // namespace common::text
```

- `parse.cpp` --- implementations extracted from emitted C++ in:
  `src/cli/generate.py` (~line 1287 for parseLong),
  `src/config/project/generate.py` (parseBool, parseInt, parseString,
  parseStringList).
- `src/common/CMakeLists.txt`: add `text/parse.cpp` to `_zith_common_sources`.
- Both generators stop emitting these functions and instead
  `#include "common/text/parse.hpp"`.

### 1.5 --- Remove local Span from types.hpp

- `src/frontend/lexer/types.hpp`: delete `struct Span`, add
  `#include "common/memory/span.hpp"`.
- `src/frontend/parser/types.hpp`: same.
- `src/frontend/ast/types.hpp`: same.
- Verify the generated headers (lexer.hpp, parser.hpp, ast.hpp) resolve
  `Span` correctly via their own includes or the build's include paths.

## Test Plan

- New test `common-diagnostic-basics`: verify `levenshteinDistance("abc", "adc") == 1`,
  `bestSuggestion` with 2 candidates, empty inputs, threshold rejection.
- New test `common-text-basics`: verify `parseInt("42") == true`,
  `parseInt("abc") == false`, `parseStringList("[a, b]")` round-trips.
- Full regression: `ctest --test-dir build --output-on-failure`. All existing
  tests must pass after the `memory::` to `common::memory::` rename, the
  physical file moves, and Span unification.
- Generator regression tests must produce byte-identical output (generated C++
  code is unchanged apart from the `#include` paths and `common::memory::`
  qualifier).

## Assumptions

- `common::memory::Span` without `FileId` is the agreed canonical form.
  File-aware APIs use `SourceSpan` from `source-map.hpp`.
- The ~130 `memory::` to `common::memory::` renames are purely mechanical.
- User-owned `types.hpp` are permitted to `#include "common/memory/span.hpp"`.
- Moving files into `common/memory/` does not break the existing include
  convention: downstream code includes `"common/memory/arena.hpp"` or the
  shorter form if the build's include paths cover the subdirectory.
