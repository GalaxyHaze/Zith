# Diagnostic Error Catalogue

## Purpose

`src/diagnostic/` is the declarative source for the compiler error catalogue. It generates
`ErrorInfo`, `lookupError`, and `ErrorTemplate` implementations used by the common diagnostic
renderer.

The runtime diagnostic engine stays in `src/common/diagnostic/`:

- `src/common/diagnostic/diagnostic.hpp` defines `Diagnostic`, `Severity`, `Note`, and suggestion
  helpers.
- `src/common/diagnostic/render.*` renders diagnostics.
- `src/common/diagnostic/levenshtein.cpp` implements Levenshtein and suggestion helpers.

## Files

| File | Responsibility |
|---|---|
| `error.rules` | Declarative error codes, fields, templates, and notes. |
| `generate.py` | Reads `error.rules` and emits `error-info.hpp/cpp`. |
| `build/src/diagnostic/error-info.hpp` | Generated error catalogue API. |
| `build/src/diagnostic/error-info.cpp` | Generated lookup and template rendering. |

## Regenerate

```bash
python3 src/diagnostic/generate.py \
  src/diagnostic/error.rules \
  --out build/src/diagnostic
```

The normal project build regenerates the catalogue automatically.

## Validation

The generator rejects duplicate codes/names, unknown sections, unknown fields, and invalid
severities. Keep generated files free of hand edits and change `error.rules` instead.

## Template Placeholders

`renderDiagnostic` provides both placeholders for catalogue templates:

- `{message}` is the `Diagnostic::message` text supplied by the call site.
- `{lexeme}` is the raw source/token text extracted from the diagnostic `span`. When the span is
  missing or outside the loaded source, `renderDiagnostic` substitutes the literal
  `<invalid span>`.

Practical examples:

| Placeholder | Rule template | `renderDiagnostic` output |
|---|---|---|
| `{message}` | `template = "{message}"` | `path:line:col: error: E4001: broken` |
| `{lexeme}` | `template = "unknown {lexeme}"` | `path:line:col: error: E4002: unknown +` |

For coded diagnostics (`code != 0`), the renderer emits a compact first line containing the
catalogue code, then a rich block headed by `  --> path:line:col`. With `contextLines > 0` the
source line, gutter, caret/tildes and rendered message follow the header, and notes appear as
`  = note: ...`. Diagnostics without a code keep their existing compact format and `note:` lines.

## Public API

The generated catalogue is in `common::diagnostic` and `build/src/diagnostic/error-info.hpp`:

- `lookupError(uint32_t code, const ErrorInfo *&out)` returns whether the code exists.
- `errorInfo(uint32_t code)` returns the matching `ErrorInfo`.
- `ErrorTemplate{info}.render(message, lexeme = {})` renders the catalogue template.
- `renderDiagnostic(FILE *, SourceMap &, const Diagnostic &, RenderOptions)` renders a complete
  source-annotated diagnostic.

## Demo

`tests/diagnostic/diagnostic-demo.cpp` looks up `E4001` and `E4002`, renders their templates, adds a
source file to `SourceMap`, and prints a rendered `E4002` diagnostic with a span.

```bash
cmake --build build --target diagnostic-demo -j
ctest --test-dir build -R '^diagnostic-demo$' --output-on-failure
```
