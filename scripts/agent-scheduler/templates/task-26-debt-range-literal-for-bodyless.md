# {AGENT}: {TASK}

## Goal

Stop `for (literal..literal){}` and `for (1>..<5){}` from reaching codegen with
an invalid HIR expression. The current path records the loop as a literal-range
`for in` even when no loop binding exists, then `lowerForIn` returns
`kInvalidHirExpr` in the body, which later crashes codegen (`run`/`build`).

## Context

- Plan: debt, range-literal `for` without a loop binding crashes.
- Worktree: {REPO}/.awt/{AGENT} (branch awt/{AGENT}).
- Follow `docs/implementation-debt.md` under "Outras incompletudes registadas" and
  `docs/impl-status.md` under `range` and `Known Debt`.
- Reproducer:

```zith
fn main(): i32 {
    for (1..5){}
    return 0;
}
```

## Recognized Root Cause

- `PerModuleSema::inferForIn` in `src/sema/sema-control.cpp` inserts
  `typed_map.forInRangeLiteral.insert(id.value)` even when
  `expr.forInBinding` is absent.
- `HirLowerModern::lowerForIn` in `src/sema/hir-lower-block.cpp` requires
  `expr.forInBinding` for the literal-range path and otherwise returns
  `kInvalidHirExpr`.
- Codegen then emits a branch whose condition is `kInvalidHirExpr`.

## Scope

1. Cause `for (literal..literal){}` without a binding to be rejected with a clear
   diagnostic before HIR codegen. Accepting it silently is not a valid fix.
2. Keep all four range spellings (`1..5`, `1>..5`, `1..<5`, `1>..<5`) working
   when a loop variable is present, and keep the `lexer`/`Dots` behavior intact.
3. Add a defensive codegen guard only if that is needed to avoid crashing on
   `kInvalidHirExpr` reachable in another path. Prefer fixing sema so the bad
   HIR is never produced.
4. Add focused regression tests for each missing-input variant and confirm they
   fail before the fix if run in isolation.

Owned files for implementation:

- Sema: `src/sema/sema-control.cpp`, `src/sema/sema-modern.hpp`.
- HIR lowering only as needed: `src/sema/hir-lower-block.cpp`.
- Optional defensive codegen guard only as needed:
  `src/codegen/codegen-emit-expr.cpp`, `src/codegen/codegen-emit-stmt.cpp`.
- Tests: `tests/test-codegen.cpp` and/or `tests/test-frontend.cpp`; do not add
  new executables.
- Docs: `docs/impl-status.md`, `docs/implementation-debt.md`. Edit only the
  section/line for this item in each shared file.

Out of scope:

- Changes to `src/frontend/frontend-stmt.cpp` loop parsing or
  `src/formatter/*`; if `parseFor()` already creates a malformed node, record
  that as a follow-up and fix it in sema instead of changing parser ownership in
  this task.
- `src/frontend/frontend-expr.cpp` range parsing and the `Dots` token.
- `src/sema/sema-cast-coerce.cpp`; the narrowing-overflow task owns it.
- Any generic loop semantics, `++`/`--`, or ownership/NRA changes.

## Acceptance Criteria

- `zithc check` rejects `for (literal..literal){}` with one targeted diagnostic.
- `zithc run`/`zithc build` on the reproducer exits cleanly with that diagnostic
  instead of segfaulting.
- Valid `for (x in 1..5)` and iterator `for (x in xs)` behavior is unchanged.
- The bad HIR expression is no longer produced for this input, so codegen cannot
  branch on `kInvalidHirExpr` for it.
- The debt/status notes record the resolved behavior and remaining test
  coverage.

## Verification

```bash
cmake --build {REPO}/build -j4
ctest --test-dir {REPO}/build -R 'range|codegen|frontend|control|loop' --output-on-failure
{REPO}/build/zithc --include stdlib check /tmp/bodyless-range.zithm
cmake --build {REPO}/build --target fmt-check
ctest --test-dir {REPO}/build --output-on-failure
```

After success:

```bash
cd {REPO}/.awt/{AGENT}
{AWT_SKILL_DIR}/scripts/awt checkin {REPO} "{AGENT}: {TASK}"
{AWT_SKILL_DIR}/scripts/awt request-merge {REPO} "{AGENT}: {TASK}"
```

## End Of Task

After requesting merge, do NOT finish this session. Return to the listening
loop: re-read `TASK.md` whenever the scheduler advances this worktree. Execute
each new task as it appears and request merge again after completing it. Stop
only when `TASK.md` contains a `# Status` section with `end` below it.
