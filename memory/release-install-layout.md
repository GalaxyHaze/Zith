# Release Install Layout Memory

Short operational pointer for the release/installer layout. The audit snapshot
with the per-platform artifact matrix and stdlib destinations is archived at
`docs/plans/archive/release-artifacts.old.md` and
`docs/plans/archive/release-stdlib.old.md`. Current release behavior lives in
`.github/workflows/`. This note keeps only the discovery contract and the
installer fixes that are easy to regress.

## Discovery Contract

`src/support/stdlib-discovery.cpp` exposes `findStdlibRoots()`, which checks in
order:

1. `ZITH_STDLIB` when it points to an existing directory.
2. `<binary_dir>/../share/zith/stdlib`.
3. `<binary_dir>/../stdlib`.

`CMakeLists.txt` installs the stdlib to
`${CMAKE_INSTALL_DATADIR}/zith/stdlib`, which for a conventional prefix is
`share/zith/stdlib`, the same root the release installer uses.

## Installer Fixes

- `scripts/install.sh` treats a version without a leading `v` as `v<version>`
  before building GitHub release URLs.
- The Unix path installs the stdlib under `/usr/local/share/zith/stdlib` and
  removes the existing directory before extraction, so stale files from an
  older stdlib bundle do not survive an upgrade.
- MSYS/MinGW/Cygwin installs the binary to `$ZITH_PREFIX/bin` and extracts the
  stdlib zip to `$ZITH_PREFIX/share/zith/stdlib`.
- The stdlib tar archive has no top-level directory, so no `--strip-components`
  flag is used. `rm -rf "$STDLIB_DIR"` before extraction is the stale-file
  protection.

## macOS Release Runtime

The macOS compiler and language server link against Homebrew LLVM libraries.
Release packaging therefore bundles their non-system Mach-O dependency closure
once per architecture in `zith-macos-runtime-{amd64,arm64}.tar.gz`. The
`build-main` job stages compiler binaries as short-lived internal artifacts.
The matching macOS `build-lsp` job downloads its compiler, packages it with
`zith-lsp`, and uploads both executables plus the shared runtime to the draft
release. Both executables resolve libraries under `../share/zith/lib`.

The Unix installer always installs this runtime on macOS, and installs the
language server only when requested with `--lsp`. PowerShell uses `-Lsp` for
the optional server. There is no musl LSP asset, so the Unix installer rejects
`--musl --lsp`. Release asset validation checks each runtime dylib against the
SHA-256 hashes in its manifest. The macOS packaging step still needs execution
on hosted macOS runners before these release changes are considered verified.

## Smoke Check

The `smoke-scoop` job must bootstrap Scoop because the hosted Windows runner
used in run `41320220775` did not provide the `scoop` command. The job uses the
upstream `get.scoop.sh` installer, verifies the shim under
`$USERPROFILE\scoop\shims`, exports that directory through `GITHUB_PATH`, sets
`CI=true`, and preserves the installer exit code. Keep checkout credentials
unpersisted while the remote installer runs.

The release workflow updates distribution metadata before `smoke-scoop` and
`publish-release`. A failed Scoop smoke can therefore leave `main` pointing at
a release that is still a draft. Treat test-tag dispatches as a public
distribution change, not as an isolated build.

```bash
zithc --include /usr/local/share/zith/stdlib check examples/hello-world.zith
```
