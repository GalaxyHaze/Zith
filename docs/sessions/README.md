# Migration Session Briefs

These briefs divide the remaining parser migration from `main` into separately
runnable agent sessions. They are ordered so each session starts from a state
produced by the previous one.

| Session | Work area | Depends on |
|---|---|---|
| 01 | Reconcile lexer token kinds and parser rules | none |
| 02 | Stabilize generated AST and parser public types | none |
| 03 | Implement handwritten recursive parser actions | 01, 02 |
| 04 | Add/rework parser parity tests | 03 |
| 05 | Wire the `Parsed` session stage and CLI check | 03 |
| 06 | Full build, full test suite, migration matrix update | 04, 05 |

## Shared Constraints

- Work in `/home/diogo/Zith-Lang`.
- Do not modify generator source under `src/*/generate.py`,
  `src/session/generate.py`, `tools/rules_kit/`, or `tools/test_kit/`.
- Do not modify `src/symbols/` or `src/common/import/`.
- Never edit files under `build/` by hand; regenerate them through CMake or the
  documented generator commands, then build.
- Use `apply_patch` for manual edits.
- The worktree is intentionally dirty. Never `git reset` or `git checkout --`
  unrelated user/prior-agent changes.
- If a required AST or parser shape cannot be expressed by the unchanged
  generators, stop and ask for explicit approval to change the generator.
- The canonical parser output API is the generated AST (`Program` root and
  generated nodes), not a `FrontendSnapshot` compatibility layer.

## Current Worktree Notes

The migration phase is complete:

- `src/frontend/lexer/generate.py` and the other generator sources are clean.
- `src/frontend/lexer/lexer.rules` uses only stock token kinds.
- `src/frontend/parser/actions.cpp` implements the recursive parser.
- `src/frontend/parser/parse.hpp` declares the token-aware entry point.
- `src/frontend/parser/types.hpp` owns the move-safe `ParseOutput`.
- `src/frontend/ast/ast.rules` defines the generic migrated node set.
- `src/session/session.rules` has a real `Parsed` stage.
- `src/session/dispatch.cpp` implements `Parsed`.
- Focused parser/session/AST tests pass, and the full `ctest` suite passes.
