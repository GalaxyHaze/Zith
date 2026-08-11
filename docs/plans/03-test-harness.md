# Step 3 --- Test Harness

## Summary

Extract the duplicated Python test helpers from all four regression test
files into a shared `tools/test_kit/` package. Each test file imports
this instead of redefining `assert_contains`, `write_rules`, etc.

Depends on Step 2 (the test harness may import from `rules_kit` if
`write_rules` or `run_generator` use it internally).

## Current Duplication

| Function | Copies | Files |
|----------|--------|-------|
| `assert_contains` | 4 | lexer, ast, parser, session |
| `assert_not_contains` | 2 | parser, session |
| `write_rules` | 3 | ast, parser, session |
| `run_generator` | 4 | lexer, ast, parser, session |
| `compile_smoke` | 3 | ast, parser, session |
| `expect_error` | 2 | ast, parser |

## API

`tools/test_kit/` package structure:

```
tools/test_kit/__init__.py
tools/test_kit/asserts.py
tools/test_kit/smoke.py
```

Public symbols:

- `assert_contains(text, needle, label)` --- raises `AssertionError` with `label`.
- `assert_not_contains(text, needle, label)` --- raises `AssertionError` with `label`.
- `write_rules(base, text) -> Path`
- `run_generator(repo_root, rules_path, out_dir, types_path=None) -> CompletedProcess`
- `compile_smoke(repo_root, compiler, include_root) -> None`
- `expect_error(repo_root, tmpdir, rules_text, needle, label) -> None`

`compile_smoke` provides a default `types.hpp` and smoke `main()` that
exercises the generated surface. Each test can override by passing a
custom `include_root`.

`expect_error` creates a temporary rules file, runs the generator, and
asserts the process fails with `needle` in stderr.

## Implementation Changes

- Create `tools/test_kit/` with `__init__.py`, `asserts.py`, `smoke.py`.
- For each regression test:
  `tests/frontend/lexer-generator-test.py`,
  `tests/frontend/ast-generator-test.py`,
  `tests/frontend/parser-generator-test.py`,
  `tests/session/session-generator-test.py`:
  - Add `sys.path` insertion pointing at `tools/`.
  - Import from `tools.test_kit`.
  - Remove the now-imported local definitions.
- Update `tests/frontend/CMakeLists.txt` and `tests/session/CMakeLists.txt`:
  add `tools/test_kit/__init__.py` etc. to `DEPENDS` of each
  `add_test(NAME ...-generator-regression)`.
- Verify all four regression tests pass.

## Test Plan

- `ctest --test-dir build -R generator-regression --output-on-failure`
- Manual test of `compile_smoke` with a deliberately broken generated
  file to ensure it catches compiler errors (the `expect_error` variant).
- Manual test of `expect_error` with a known-invalid rules file to
  confirm `needle` matching in stderr.

## Assumptions

- The test harness does not depend on pytest or unittest. Bare `assert`
  statements and `subprocess` are sufficient.
- `compile_smoke` uses the same `types.hpp` shape as the current
  regression tests; new subsystems that need different types can pass a
  custom `include_root`.
- `expect_error` creates temporary `types.hpp` matching the shape used
  by the generators under test (sample types only).
