# {AGENT}: {TASK}

## Goal

Implement Zith-- platform-specific imports according to
`docs/plans/platform-imports.md`. The compiler main targets the sibling
language `Zith--`; do not implement features specified only for Zith.

## Context

- Plan: platform imports and module resolution.
- Worktree: {REPO}/.awt/{AGENT} (branch awt/{AGENT}).
- Primary files: `src/session/frontend-context.{hpp,cpp}`,
  `tests/test-frontend-context.cpp`, and `tests/test-frontend-modern-pipeline.cpp`.
- The resolver candidates are `foo.<arch>.<os>.zith`, `foo.<arch>.zith`,
  `foo.<os>.zith`, then `foo.zith`.
- Cache keys already include `targetTriple`; keep generic-vs-variant behavior
  deterministic and LLVM-independent where possible.

## Scope

1. Add a target-component helper for arch/os, or reuse existing target fields
   from `FrontendConfig` when sufficient.
2. Modify `FrontendContext::resolveImport()` so literal imports still work and
   platform variants are considered before the generic fallback.
3. Add failure diagnostics that mention the missing generic or platform
   variants when no candidate exists.
4. Add tests for exact arch.os, arch-only, os-only, generic fallback, missing
   all variants, and target-specific cache separation.
5. Update `docs/plans/0.7.0/README.md`, `docs/roadmap.md`,
   `docs/Zith--.md`, `docs/Zith---implementation.md`, `docs/impl-status.md`,
   and `memory/platform-imports.md` as behavior allows.

Out of scope:

- New import syntax, macro/conditional syntax, comptime, or vendor/env variants.
- Editing `src/session/compilation-session.cpp` in a way that changes the
  scheduler-owned split.

## Acceptance Criteria

- Candidate order matches the plan exactly.
- Generic fallback remains mandatory; no variant can replace the absence of
  `foo.zith`.
- No parser or sema behavior changes outside import resolution.
- Target-specific import tests pass and diagnostics are actionable.

## Verification

```bash
cmake --build {REPO}/build -j4
ctest --test-dir {REPO}/build -R 'frontend' --output-on-failure
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
