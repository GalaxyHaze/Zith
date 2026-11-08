# {AGENT}: {TASK}

## Goal

Audit the Homebrew tap side of the release story, verify that the formula
contract in `update-package.yml` matches current source archives and builds,
and produce an actionable report/debt entry for anything that cannot be tested
locally.

## Context

- Plan: Homebrew packaging audit.
- Worktree: {REPO}/.awt/{AGENT} (branch awt/{AGENT}).
- The compiler main targets `Zith--`, a sibling language to Zith. Zith-specific
  compiler artifacts are out of scope.
- The current repo does not contain a Homebrew formula; the release automation
   dispatches `homebrew-zithc` events from `.github/workflows/update-package.yml`.
- Homebrew formulas typically need a `sha256` for a source tarball URL and
   should install the stdlib through `install -d`/`cp` or a bundle layout.

## Scope

1. Read `.github/workflows/update-package.yml`,
   `.github/workflows/build-artifact.yml`, `CMakeLists.txt`, and
   `src/support/stdlib-discovery.cpp`.
2. Summarize the intended Homebrew formula (source tarball, tagged archive,
   based on the repo owner/version).
3. Audit the data the workflow sends to `GalaxyHaze/homebrew-zithc`: version,
   source URL, and SHA-256. Note bugs such as tag/version mismatches,
   unreachable repository/owner assumptions, or missing architecture handling.
4. Evaluate whether the formula should build from source or install a release
   binary, and how the stdlib should be installed so `findStdlibRoots()`
   locates it.
5. Write the recommended formula stub under `.github/homebrew/` or the existing
   packaging docs, and add the missing work to `docs/implementation-debt.md` if
   a live tap cannot be validated.

Out of scope:

- Creating/pushing an external `homebrew-zithc` repository.
- Changing the release workflow.
- Editing Window/Linux installers.

## Acceptance Criteria

- The audit states whether the formula is current, stale, or blocked by an
   external tap state.
- The recommended formula layout includes the stdlib in a Homebrew-compatible
   path and the compiler is configured the way local CMake expects.
- Any invented version/hash is flagged as a placeholder, never presented as a
   verified value.

## Verification

```bash
cd {REPO}/.awt/{AGENT}
rg -n "homebrew|sha256|archive/refs/tags|stdlib" .github/workflows/update-package.yml CMakeLists.txt src/support/stdlib-discovery.cpp
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
