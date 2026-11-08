# {AGENT}: {TASK}

## Goal

Audit `.github/workflows/build-artifact.yml` so release artifacts are built with
the intended feature flags, especially `ZITH_HAS_LLVM`, and all release files
that users download are consistent with what the project claims to ship.

## Context

- Plan: build artifact/feature flag audit.
- Worktree: {REPO}/.awt/{AGENT} (branch awt/{AGENT}).
- The compiler main targets `Zith--`, a sibling language to Zith. Zith-specific
  compiler artifacts are out of scope.
- Current release matrix heavily passes `-DZITH_HAS_LLVM=OFF` for `build-main`
   and `build-musl`, while some LSP matrix entries and the local development
   build use LLVM.
- `CMakeLists.txt` auto-disables LLVM when LLVM 18+ is not found, and keeps
   codegen/example tests behind `ZITH_HAS_LLVM`.

## Scope

1. Read `.github/workflows/build-artifact.yml` and `CMakeLists.txt`.
2. Build a per-target matrix table in the task body or a new markdown doc
   showing: runner, target_name, `ZITH_HAS_LLVM`, `ZITH_ENABLE_FFI`,
   `ZITH_ENABLE_C_COMPILE`, `ZITH_IS_WASM`, and whether `zithc`/`zith-lsp`
   artifacts are produced.
3. Identify flags that contradict the intended release configuration or the
   project docs/ADR. Mark only mismatches that are demonstrably stale.
4. For each mismatch, either fix the workflow flag or document the decision in
   `docs/implementation-debt.md`/`memory/` with a short rationale.
5. Check release smoke paths: does the artifact upload use the same name as
   `scripts/install.sh`, `scripts/install.ps1`, Scoop, Homebrew, and the
   stdlib archive naming?

Out of scope:

- Rebuilding artifacts locally.
- Editing installer scripts or package manifests.
- Implementing compiler features.

## Acceptance Criteria

- The matrix table is committed in the task body (if it is the task text) or
   in a small `docs/plans/release-artifacts.md` file.
- Every `-DZITH_HAS_LLVM=OFF` in `build-main`/`build-musl` is either fixed,
   labeled intentional, or listed as a blocker with a concrete follow-up.
- Uploaded artifact names are reconciled with installer expectations where the
   fix is in this workflow.

## Verification

```bash
cd {REPO}/.awt/{AGENT}
rg -n "ZITH_HAS_LLVM|ZITH_ENABLE_FFI|ZITH_ENABLE_C_COMPILE|zithc-stdlib|target_name" .github/workflows/build-artifact.yml
```

After verification:

```bash
{AWT_SKILL_DIR}/scripts/awt checkin {REPO} "{AGENT}: {TASK}"
{AWT_SKILL_DIR}/scripts/awt request-merge {REPO} "{AGENT}: {TASK}"
```

## End Of Task

After requesting merge, do NOT finish this session. Return to the listening
loop: re-read `TASK.md` whenever the scheduler advances this worktree. Execute
each new task as it appears and request merge again after completing it. Stop
only when `TASK.md` contains a `# Status` section with `end` below it.
