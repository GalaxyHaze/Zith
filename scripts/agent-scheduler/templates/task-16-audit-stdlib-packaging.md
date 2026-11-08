# {AGENT}: {TASK}

## Goal

Audit how stdlib is packaged, uploaded, downloaded, and discovered, then remove
the gaps between the release assets, the installers, and the compiler's stdlib
discovery on each platform.

## Context

- Plan: stdlib packaging/discovery audit.
- Worktree: {REPO}/.awt/{AGENT} (branch awt/{AGENT}).
- The compiler main targets Zith--; artifacts specific to Zith are out of scope.
- `CMakeLists.txt` now installs `stdlib/` to
  `${CMAKE_INSTALL_LIBDIR}/zith/stdlib`.
- `const std::vector<std::string> findStdlibRoots()` looks for
  `<binary_dir>/../share/zith/stdlib` and `<binary_dir>/../stdlib`.
- `.github/workflows/build-artifact.yml` uploads `zithc-stdlib-<tag>.tar.gz` and
  `zithc-stdlib-<tag>.zip` built from the repo `stdlib/` directory.

## Scope

1. Read `CMakeLists.txt`, `src/support/stdlib-discovery.cpp`,
   `.github/workflows/build-artifact.yml`, `scripts/install.sh`,
   `scripts/install.ps1`, and `.github/scoop/bucket/zithc.json`.
2. Produce a per-platform mapping: binary location, stdlib archive layout,
   extraction destination, and discovery root found by `findStdlibRoots()`.
3. Fix deterministic mismatches that can be verified from the repo: archive
   name/tag commas, zipped directory nesting, extraction to a stale location,
   or install path that never matches discovery.
4. Add/update a short `docs/plans/release-stdlib.md` or
   `memory/release-stdlib.md` documenting the intended cross-platform layout
   and the smoking commands a user can run to verify stdlib discovery.
5. If a full fix requires coordinated workflow/installer changes from other
   agents, note the dependency in `docs/implementation-debt.md` instead of
   fabricating a single-file solution.

Out of scope:

- Changing the stdlib content or language semantics.
- Changing `findStdlibRoots()` search order without a release-layout rationale.
- Editing unrelated compiler features.

## Acceptance Criteria

- The final mapping says exactly which release artifact ends up at which
   discovery root per platform.
- Repo-verifiable fixes are applied in this branch.
- External/CI-only verification gaps are listed as debt or follow-up risk, not
   hidden.

## Verification

```bash
cd {REPO}/.awt/{AGENT}
rg -n "zithc-stdlib|share/zith|stdlib|CMAKE_INSTALL_LIBDIR" CMakeLists.txt src/support/stdlib-discovery.cpp .github/workflows/build-artifact.yml scripts/install.sh scripts/install.ps1
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
