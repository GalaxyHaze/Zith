# Step 2 --- Python Rules Kit

## Summary

Extract the duplicated Python primitives from all six generators into a
shared `tools/rules_kit/` package. Each `generate.py` imports this instead
of redefining `strip_comment`, `cpp_string`, `RuleError`, etc.

## Current Duplication

| Function | Copies | Files |
|----------|--------|-------|
| `strip_comment` | 6 | cli, config/project, lexer, ast, parser, session |
| `cpp_string` | 6 | cli, config/project, lexer, ast, parser, session |
| `split_top_level` | 3 | config/project, lexer, parser |
| `to_camel` | 2 | cli, config/project |
| `parse_quoted` | 2 | lexer, parser |
| `validate_cpp_type` | 2 | parser, session |
| `RuleError` + join lines | 4 | lexer, ast, parser, session |

Each generator also reimplements logical-line joining (continuation when
`_balanced`/`balanced` fails) and `Path` scaffolding.

## API

`tools/rules_kit/` package structure:

```
tools/rules_kit/__init__.py    # re-exports public API
tools/rules_kit/errors.py      # RuleError, render_error
tools/rules_kit/text.py        # strip_comment, cpp_string, cpp_char,
                               #   split_top_level, parse_quoted, to_camel,
                               #   validate_cpp_type, validate_identifier,
                               #   is_balanced, join_logical_lines
```

Public symbols (importable from `tools.rules_kit`):

- `RuleError(line_no: int, message: str)` --- exception with `render(path)` method.
- `strip_comment(line: str) -> str`
- `cpp_string(value: str) -> str`
- `cpp_char(value: str) -> str`
- `split_top_level(body: str, line_no: int) -> list[str]`
- `parse_quoted(raw: str, line_no: int) -> str`
- `to_camel(name: str) -> str`
- `validate_cpp_type(value: str, line_no: int, label: str) -> None`
- `validate_identifier(value: str, line_no: int, label: str) -> None`
- `is_balanced(body: str) -> bool`
- `join_logical_lines(text: str) -> list[tuple[int, str]]` --- parses raw text
  into `(line_no, logical_line)` pairs, handling comment stripping and
  continuation joining.

`join_logical_lines` is the common implementation of the pattern where each
generator strips comments, accumulates continuation lines until `balanced`
returns true, and then yields the joined logical line. It replaces the
duplicated loop in lexer, ast, parser, and session generators.

## Implementation Changes

- Create `tools/rules_kit/` with `__init__.py`, `errors.py`, `text.py`.
- For each generator:
  `src/cli/generate.py`,
  `src/config/project/generate.py`,
  `src/frontend/lexer/generate.py`,
  `src/frontend/ast/generate.py`,
  `src/frontend/parser/generate.py`,
  `src/session/generate.py`:
  - Add `sys.path.insert(0, ...)` pointing at `tools/`.
  - Import from `tools.rules_kit`.
  - Remove local definitions of the imported functions.
  - Replace the inline logical-line accumulation loop with
    `join_logical_lines(text)`.
- CMake: add `tools/rules_kit/__init__.py`, `tools/rules_kit/text.py`,
  `tools/rules_kit/errors.py` to each `DEPENDS` list in the generator
  custom commands so regeneration triggers on kit changes.
- Verify: run each generator and diff the output against the pre-refactor
  generated files. Output must be byte-identical.

## Test Plan

- Run each generator regression test:
  `ctest --test-dir build -R generator-regression --output-on-failure`.
- Manual diff: stash generated files before refactor, regenerate, diff.
- All six generators must produce byte-identical output.

## Assumptions

- `tools/` is in the repo root and is a Python-only support directory
  (not a CMake compiled target).
- Generators are invoked by CMake from the repo root, so `sys.path`
  insertion relative to `__file__` works.
- `join_logical_lines` uses `is_balanced` as defined in `text.py`. If
  any generator uses a variant `_balanced`, it must migrate to the
  shared implementation.
