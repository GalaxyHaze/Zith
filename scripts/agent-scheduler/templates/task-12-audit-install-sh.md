# {AGENT}: {TASK}

## Goal

Audit `scripts/install.sh` and the release layout it relies on, then update the
installer so a released `zithc` binary finds the standard library in the
installed location without requiring `ZITH_STDLIB`.

## Context

- Plan: package/installer audit.
- Worktree: {REPO}/.awt/{AGENT} (branch awt/{AGENT}).
- The compiler main targets `Zith--`, a sibling language to Zith. Zith-specific
  compiler artifacts are out of scope.
- `scripts/install.sh` currently downloads `zithc-*`, copies only the binary,
  then separately downloads `zithc-stdlib-<version>.tar.gz`.
- `src/support/stdlib-discovery.cpp` looks for
  `<binary_dir>/../share/zith/stdlib` and `<binary_dir>/../stdlib`.
- `CMakeLists.txt` installs stdlib to `${CMAKE_INSTALL_LIBDIR}/zith/stdlib`,
  not to a fixed `share` path.

## Scope

1. Read `scripts/install.sh`, `src/support/stdlib-discovery.cpp`, the install
   rules in `CMakeLists.txt`, and `.github/workflows/build-artifact.yml`.
2. Document the current behavior: which platforms, which asset names, where the
   binary and stdlib land, and whether the installed path matches stdlib
   discovery.
3. Fix installer inconsistencies that are demonstrably wrong for the release
   layout, for example mismatched stdlib paths, broken version fallback, or
   missing handling for a/no existing stdlib directory.
4. Keep the installer POSIX shell-friendly: no Bash-only features beyond the
   current `#!/bin/bash` contract, no network calls beyond GitHub release
   assets, no sudo prompts unrelated to the existing install flow.

Out of scope:

- Changing the release workflow itself.
- Refactoring `src/support/stdlib-discovery.cpp` semantics.
- Adding Homebrew/Scoop support to this installer.

## Acceptance Criteria

- The installer's stdlib destination is consistent with at least one root
   returned by `findStdlibRoots()` for the installed binary.
- Archive extraction handles an existing stdlib directory without leaving
   stale files from older versions.
- Non-musl, musl, and Windows-specific paths in the installer are audited and
   either fixed or documented as intentional.
- The task records a short Change Log section in its commit message or body.

## Verification

```bash
cd {REPO}/.awt/{AGENT}
bash -n scripts/install.sh
rg -n "share/zith/stdlib|zithc-stdlib|/usr/local" scripts/install.sh
```

```text
Expected: the script parses with bash -n and the stdlib destination matches
the documented discovery path.
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
