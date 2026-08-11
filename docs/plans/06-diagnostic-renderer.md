# Step 6 --- Diagnostic Renderer

## Summary

Add a generic diagnostic renderer that converts `Diagnostic{span, severity,
message}` into human-readable `path:line:col: severity: message` output with
source context and a caret. Built on `common::diagnostic` and `memory::SourceMap`.

Depends on Step 1.

## API

New files: `src/common/diagnostic/render.hpp` + `render.cpp`

```cpp
namespace common::diagnostic {

struct RenderOptions {
    bool useColor = false;      // if true, use ANSI escape codes
    unsigned contextLines = 0;  // lines of context before and after the error line
};

// Render a single diagnostic to `out`.
// sourceMap must outlive the call.
void renderDiagnostic(
    FILE *out,
    const memory::SourceMap &sourceMap,
    const Diagnostic &diag,
    const RenderOptions &options = {}
);

// Render all diagnostics in order, separated by blank lines.
// Stops rendering after `maxErrors` (0 = unlimited).
void renderDiagnostics(
    FILE *out,
    const memory::SourceMap &sourceMap,
    const memory::DynArray<Diagnostic> &diagnostics,
    const RenderOptions &options = {},
    unsigned maxErrors = 0
);

} // namespace common::diagnostic
```

Output format (monochrome):

```
src/main.zith:3:12: error: unexpected token '+'
  let x = a + ;
            ^
```

With `contextLines = 1`:

```
src/main.zith:3:12: error: unexpected token '+'
  2 | let y = 0 ;
  3 | let x = a + ;
    |            ^
  4 | let z = 0 ;
```

## Implementation

`src/common/diagnostic/render.cpp`:

- `renderDiagnostic`:
  1. Resolve file path from `SourceMap` and `SourceSpan`.
  2. Compute `Loc` (1-based line, 1-based col) from `sourceMap.loc()`.
  3. Print `path:line:col: severity: message`.
  4. If `contextLines > 0`, fetch surrounding lines via `sourceMap.snippet()`.
  5. Print the error line (and context lines) with line number gutter.
  6. Print caret (`^`) aligned with the column, optionally with `~~~~`
     underline for multi-char spans.
- Internal helpers: `strRepeat(char, size_t)`, `padLineNo(unsigned, unsigned)`.
- Color support: severity maps to ANSI codes (Error=red, Warning=yellow,
  Note=cyan), reset with `\x1b[0m`. Only emitted when `useColor = true`.
- `renderDiagnostics`: iterates `diagnostics`, prints blank line between
  entries. Stops after `maxErrors` if nonzero.

### Diagnostic type alignment

`renderDiagnostic` works with `common::diagnostic::Diagnostic` (from Step 1).
The session `Diagnostic` emitted by `session/generate.py` and the parser
`DiagnosticAlias` must either be `common::diagnostic::Diagnostic` or provide
a conversion. This step assumes Step 1 has already aligned the types:
- Session generated code uses `common::diagnostic::Diagnostic`.
- Parser `DiagnosticAlias` can be any type satisfying `ParserDiagnostic`
  concept; the renderer works with `common::diagnostic::Diagnostic` directly.

## Integration Points

- `src/session/dispatch.cpp`: replace ad-hoc `Diagnostic{.span = {...}}`
  with `common::diagnostic::Diagnostic`.
- Future: CLI `check` command calls `renderDiagnostics(stderr, sourceMap, ...)`
  after parse failure. Not wired in this step.
- `src/common/CMakeLists.txt`: add `diagnostic/render.cpp` to
  `_zith_common_sources`.

## Test Plan

- `diagnostic-render-test`: new test executable in `tests/common/`.
  - Load a known source string into `SourceMap`.
  - Construct a `Diagnostic` at a known line and column.
  - Call `renderDiagnostic` into a `FILE*` from `tmpfile()`.
  - Assert output contains: `path:line:col: error: message`.
  - Assert caret `^` appears at the correct column offset.
  - Assert no ANSI codes when `useColor = false`.
  - Assert `contextLines = 1` produces `n-1 |` and `n+1 |` context lines
    with `|` gutter.
  - Assert `maxErrors` caps `renderDiagnostics` output.
- `cmake --build build -j && ctest --test-dir build --output-on-failure`.

## Assumptions

- `SourceMap` already provides `loc(SourceSpan)` returning `Loc{line, col}`.
  `SourceSpan` was introduced in Step 1.
- Terminal width is not auto-wrapped; long lines overflow. The caret is
  a simple `^` at column start.
- Context gutter uses ASCII `|` for portability (not Unicode box-drawing).
- The renderer does not depend on terminal detection; `useColor` is
  opt-in via `RenderOptions`.
- Multi-byte / UTF-8 column alignment is not handled in v1. The column
  offset is a byte offset.
