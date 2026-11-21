# {AGENT}: {TASK}

## Goal

Make imported trait/interface conformance deterministic in populated workdirs,
eliminating dependence on module/symbol resolution order.

## Context

- Plan: debt, traits/interfaces importadas têm conformance instável em workdirs
  populados.
- Worktree: {REPO}/.awt/{AGENT} (branch awt/{AGENT}).
- Follow `docs/implementation-debt.md`, section "Traits/interfaces importadas
  têm conformance instável", and `memory/stdlib-allocator-ownership.md` for the
  `InPlace`/allocator context.
- `tests/test-generic-hashmap.cpp` covers `implement Box as InPlace` in a
  temporary workdir; `examples/inplace-simple.zithm` currently re-implements the
  trait locally instead of exercising the imported trait.

## Scope

1. Build a deterministic reproducer with a populated workdir containing the
   imported trait plus several modules/symbols, so the unstable qualified
   conformance path is reproducible without external state.
2. Fix the resolver/sema path so imported trait/interface conformance is
   independent of processing/import order while preserving validation rules.
3. Convert the coverage so at least one `examples/` or focused test uses the
   real imported trait instead of a local duplicate, when that is safe.
4. Update the memory note and debt entry with the root cause that was removed.

Owned files for implementation:

- Import/symbol resolution: `src/session/frontend-symbol-resolution.cpp`,
  `src/session/frontend-context.cpp`, `src/session/frontend-module-analysis.cpp`,
  `src/session/frontend-context-internal.hpp`.
- Sema conformance/method resolution: `src/sema/sema-type.cpp`,
  `src/sema/sema-method.cpp`, `src/sema/sema-decl.cpp`.
- Tests: `tests/test-interface-satisfaction.cpp`; build the populated workdir
  fixture inside that test rather than adding new executables.
- Examples and stdlib only to exercise the imported trait:
  `examples/inplace-simple.zithm`, `stdlib/std/new.zithm`.
- Memory/docs: `memory/stdlib-allocator-ownership.md`,
  `docs/implementation-debt.md`, `docs/impl-status.md`.
  Edit only the section/line for this item in each shared file.

Out of scope:

- Nominal type syntax; do not touch `src/frontend/ast-lowerer.cpp` or
  `src/frontend/frontend-decl.cpp`.
- Numeric narrowing diagnostics; do not touch `src/sema/sema-cast-coerce.cpp`.
- Opaque canonical registry; do not touch `src/cache/*`,
  `src/session/persistent-cache.cpp`.
- Ownership/NRA proof changes beyond the import-conformance reproducer.

## Acceptance Criteria

- The reproducer passes in clean, cached, and populated workdir states.
- Qualified trait calls over the imported trait resolve consistently regardless
  of module iteration order.
- Existing single-trait, interface-satisfaction, generic-hashmap, and allocator
  tests pass unchanged where the language behavior did not change.
- The debt note records the removed root cause and any remaining known gap.

## Verification

```bash
cmake --build {REPO}/build -j4
ctest --test-dir {REPO}/build -R 'trait|interface|generic|hashmap|cache' --output-on-failure
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
