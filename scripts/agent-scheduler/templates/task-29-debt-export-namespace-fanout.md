# {AGENT}: {TASK}

## Goal

Make the stdlib facade re-export surface resolve completely: when a module
exports several modules beneath one prefix, consumers must be able to derive
every intermediate namespace segment, not only the first deduplicated root.

## Context

- Plan: debt, `export` facades with shared prefix.
- Worktree: {REPO}/.awt/{AGENT} (branch awt/{AGENT}).
- Follow `docs/implementation-debt.md` section 11 and
  `docs/impl-status.md` under `Import resolution` and `Known Debt`.
- `stdlib/std/memory.zithm` re-exports `in-place`, `allocator`, `heap`, and `new`
  from `from std/memory`.
- Current behavior keeps one `ResolutionKind::ModuleAlias` per root prefix,
  e.g. `std`, when multiple exports share it. The requested future action is
  either a namespace map per prefix in `FrontendContext` or independent aliases
  for each import segment.
- Failure target: `import std/memory` must resolve
  `std.memory.in-place.InPlace` and `std.memory.allocators.heap.HeapAllocator`
  deterministically, including in populated workdirs and cached modules.

## Scope

1. Reproduce the missing fanout with a small temporary-workdir fixture that
   exercises `export std/memory` style re-export plus `import std/memory`.
2. Choose and implement the least invasive representation change in resolver
   state: independent segment aliases or a namespace map per prefix.
3. Make the exported qualified paths resolve in expression, type, constructor,
   method, and macro qualification paths without changing the public-symbol
   injection contract.
4. Keep `import Path`, `import Path as name`, `from Path`, and plain `export`
   current behavior stable.
5. Update docs and the debt entry after the fix, naming the representation
   chosen and any remaining limitation.

Owned files for implementation:

- Resolver/module analysis: `src/session/frontend-symbol-resolution.cpp`,
  `src/session/frontend-module-analysis.cpp`,
  `src/session/frontend-context.cpp`, `src/session/frontend-context.hpp`,
  `src/session/frontend-context-internal.hpp`.
- Tests: `tests/test-frontend-context.cpp`,
  `tests/test-interface-satisfaction.cpp`, or an existing CTest that can host a
  temporary-workdir fixture; do not add new executables unless unavoidable.
- Stdlib only where the missing alias prevents a covered path:
  `stdlib/std/memory.zithm` when the fix proves the alembic is incomplete.
- Docs/memory: `docs/implementation-debt.md`, `docs/impl-status.md`,
  `memory/stdlib-allocator-ownership.md` if it records the facade behavior.
  Edit only the section/line for this item in each shared file.

Out of scope:

- `src/sema/*` and `src/codegen/*`; this task is resolver-only unless a sema
  call-site relies on `modulePath` being populated.
- `src/sema/sema-cast-coerce.cpp` and numeric narrowing.
- Pointer narrowing, range `for`, and discard-only expression statements; their
  agents own those files in this wave.
- Adding `import`/`export` syntax or changing visibility rules.

## Acceptance Criteria

- A fixture comparable to `export std/memory` resolves both
  `std.memory.in-place.InPlace` and a second full path under the same prefix.
- The same fixture passes in clean and cached/populated workdir states.
- Diagnostic-free consumers keep only the symbols they were given: `import`
  still does not inject bare last-segment names, and `from`/`export` still
  inject public symbols.
- Existing import/export/temp-workdir tests pass unchanged.
- The debt entry records the fixed model.

## Verification

```bash
cmake --build {REPO}/build -j4
ctest --test-dir {REPO}/build -R 'frontend|context|import|export|interface|generic|memory|cache' --output-on-failure
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
