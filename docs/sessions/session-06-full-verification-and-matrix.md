# Session 06 - Full Verification and Migration Matrix Update

## Context

Sessions 01-05 should have produced:

- A lexer with stock token kinds and full `main` keyword/operator coverage.
- Generated AST nodes with parser public types.
- Handwritten recursive parser actions in `actions.cpp`.
- Parser parity tests.
- A real `Parsed` session stage and `zithc check` target.

## Goal

Run the full build and test suite, fix any remaining integration failures, and
update the migration matrix only after verification passes.

## Out of Scope

Do not start sema, import resolution, macro expansion, or backend implementation.
Leave those rows as future work.

## Files You May Edit

- `docs/main-migration-matrix.md`
- Any non-generated source required to pass the build/tests in this final pass.

## Steps

1. Confirm no forbidden generator/subsystem diffs are introduced:
   ```bash
   git diff -- src/frontend/lexer/generate.py src/frontend/ast/generate.py src/frontend/parser/generate.py src/session/generate.py
   git diff -- src/symbols src/common/import
   ```
   The first command must be empty after Session 01. If it is not, fix it before
   proceeding.
2. Build all targets:
   ```bash
   cmake --build build -j
   ```
3. Run the full suite:
   ```bash
   ctest --test-dir build --output-on-failure
   ```
4. If failures remain, fix them in the appropriate non-generated source and
   rerun. Never patch `build/` output directly.
5. Run a representative parser parity file through the CLI:
   ```bash
   ./build/zithc/zithc --verbose check /tmp/zith-parity-check.zith
   ```
6. Update `docs/main-migration-matrix.md`:
   - Mark the parser migration row(s) complete only after the full suite passes.
   - Row 4 (`for`, `when`, ranges, arrays, structs, optionals, casts, null
     checks) is marked parser-level complete with sema/backend work explicitly
     listed as future.
   - Rows covering imports/symbols/macros/sema/codegen remain Not started or
     future work.
   - Add a status note that the migration in this phase is parser/lexer/AST only.
7. Record the final focused test command outputs if they were run, and include
   the full `ctest` summary in the session close-out.

## Actual Verification

- `cmake --build build -j` succeeds.
- `ctest --test-dir build --output-on-failure`: 51/51 tests pass.
- Focused runs pass for `session|parser` and `ast-`.
- `git diff` shows no generator source edits and no protected subsystem edits.
- `zithc check` reaches `Parsed` for supported syntax.

## Acceptance Criteria

- `cmake --build build -j` succeeds.
- `ctest --test-dir build --output-on-failure` passes.
- `git diff` shows no generator source edits and no protected subsystem edits.
- `zithc check` reaches `Parsed` for supported syntax.
- `docs/main-migration-matrix.md` reflects parser work complete and sema/macro/
   import/backend work as future.

## Final Close-Out Notes

The close-out should explicitly say which areas are complete:

- Lexer parity: complete.
- Generated AST parity: complete.
- Parser parity and recovery: complete.
- Session `Parsed` stage: complete.
- `zithc check` to `Parsed`: complete.

And which remain future:

- Sema/type checking.
- Import resolution and import graph.
- Macro expansion.
- Symbols.
- HIR/codegen/backends.
