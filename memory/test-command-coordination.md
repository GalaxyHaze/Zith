# Native `zithc test`: split between master and hermes agents

This note records how the native `zithc test` work is divided across agents and
which files each side owns, so a future session can resume the coordination
without re-deriving it. The design itself is in `docs/plans/test-command.md`
and `docs/adr/0045-native-test-command-and-driver.md`. The current implementation
state below was checked on 2026-10-27.

## Split

The master (this checkout, `main`) keeps the hard and risky steps. Hermes
worktrees take the long, mechanical steps. The plan is mostly serial, so the
real parallelism is only Wave 0 and the tail.

Master owns, in order:

- Step 3: HIR lowering of `@assert`/`@log` with the in-test versus out-of-test
  branch.
- Step 4: synthesis of each test's result slot and the HIR driver.
- Step 5: the `Debug` section format and the location table.
- Steps 6 and 7: the rewritten `test()` command and the report modes.

hermes1 owns Wave 0 (frontend surface): `AttributeKind::Test`,
`classifyAttribute`, and the `assert`/`log` intrinsic names and parse branch.
Files: `src/frontend/frontend.hpp`, `src/frontend/attribute-classify.hpp`,
`src/frontend/ast-lowerer.hpp`, `src/frontend/frontend-expr-primary.cpp`.

hermes2 was assigned `std/test` (thin wrappers over `@assert`), followed by the
example suite under `tests/zith/` and its CTest registration. That task was not
started in its worktree.

## File ownership boundary

This boundary prevents worktree collisions. The master keeps `src/sema/`,
`src/hir/`, `src/zirl/`, `src/session/`, `src/cli/`. hermes1 keeps
`src/frontend/` and `sema-cast-coerce.cpp`/`sema-modern.hpp` only if the master
has not yet touched them. hermes2 keeps `stdlib/std/test.zithm`.

Shared files that need a single writer at merge time: `CMakeLists.txt` (CTest
registration) and any stdlib module list. The master merges those.

## Verification gates

1. Wave 0 to Wave 1: `zithc check` accepts `#[test] fn f(): void { @assert(true); }`,
   rejects `#[test]` on non-functions with `W2029`, and `fmt-check` is green.
2. Wave 1 to Wave 2: a failing `@assert` writes the correct result slot and the
   `Debug` section resolves the location; the C++ suite is green.
3. Wave 2 to Wave 3: `zithc test` runs the suite and reports per test on all
   three backends; the C++ suite and `fmt-check` are green.

## Worktree state

Worktrees live at `.awt/hermes1` (branch `awt/hermes1`) and `.awt/hermes2`
(branch `awt/hermes2`), both created from `d3978d54`. Each has a `TASK.md` and
an `agent-id`. `.awt/` is git-ignored, so checkin never commits them.

hermes1 (frontend surface) is merged into `main` as `84303dba`. Its branch
was rebased on `9e71b8ce` before the fast-forward merge. The merged commit adds
`AttributeKind::Test`, the `test` classify branch, the `assert`/`log` intrinsic
names, and the one-operand parse branch. Gate 0 to 1 passed: `zithc check`
accepts `#[test] fn f(): void { @assert(true); @log("x"); }`, `#[test]` on a
non-function is `W2029`, an unknown attribute is `W2028`, and `test-frontend`
is 402 passed / 0 failed. The hermes2 worktree remains at the plan commit
`d3978d54`; it has no implementation changes.

## Wave 1 (plan steps 3-5) is closed

Master commit `b112620a` lands steps 3-5:

- Step 3: `@assert`/`@log` lowering in `src/sema/hir-lower-test.cpp`, dispatched
  from `lowerLayoutIntrinsic`. `@assert` outside a test lowers to a panic;
  `@log` writes directly to stdout via `puts`, not to a synthesized log buffer.
- Step 4: `buildTestDriver` synthesizes the `main` driver in HIR.
- Step 5: the `Debug` ZIRL section carries `(test, assert id) -> path:line:col`.

Gate 1 to 2 passed: a failing `@assert` writes the correct result slot and the
`Debug` section resolves the location, identical on native LLVM, VM v2, and the
interpreter, and the C++ suite is green except the pre-existing
`docs-consistency` failure (the `README.md` in `HEAD` has no `## Compilation
Pipeline` section, unrelated to this work).

Two things to know when resuming:

- `site.span.file` in `TestAssertSite` is a frontend file id, so
  `persistent-cache.cpp` maps it through `mSnapshotDiagnosticFiles` before
  resolving it against the session `SourceMap`. Without that the location is
  only correct for single-module builds.
- The `@log` string-literal fast path (a `*char` literal instead of a slice)
  exists because the VM and interpreter cannot lower a slice-typed literal.
  Keep it when extending `@log`.

## Wave 2 and stdlib coverage are implemented

The runner in `src/cli/cmd/tool.cpp` executes each test file through the
synthesized driver, supports filtering, reports assertion locations, and skips
files without tests. `std/test` exposes assertion macros that preserve the
`#[test]` context and format failure details through `FormatBuffer`. It now
also provides membership, optional-presence, and floating-point tolerance
helpers.
`test-zith-test-suite` runs the full `tests/zith/` assertion fixture on native
and VM v2. It also checks bounded logs and a minimal `@assert`/`@log` fixture
on the HIR interpreter. The rich fixture is not run on HIR because its current
formatting path fails there; omitting that expectation does not establish
full-suite support on all three backends.

On 2026-10-27, native and VM v2 both produced `2 passed; 2 failed; 4 total`
for the full fixture, with exit code 1 for the two intentional failures.
Integration coverage now compares their complete stdout and stderr, including
the formatted `assertEq` and `fail` messages and assert locations. HIR execution
of the rich fixture still fails: `user_defined_formatable` is reported as a
failed assertion, and `assertEq`/`fail` output does not preserve the formatted
message. Per the user's direction, further HIR interpreter diagnosis is
deferred. The all-backend acceptance criterion remains open.

On 2026-10-28, the expanded fixture contains 16 tests: native and VM v2
produce `6 passed; 10 failed; 16 total`, where the failures are intentional
assertion cases. The fixture covers equality, membership, one-of selection
over arrays and slices, optional presence, float tolerance, and explicit
failure.

On 2026-10-29, `@check(cond)` was added as an accumulating failure primitive.
Inside `#[test]`, each false check emits a failure event and execution
continues. `@assert` remains fail-fast, while prior checks are retained if a
later assert ends the test. Check and assert sites share per-test failure IDs
and the ZIRL `Debug` location table. Outside a test's lexical body, `@check`
uses the same panic behavior as `@assert`; standard-library helpers therefore
remain macros expanded at the caller. Native, VM v2, and HIR integration
coverage verifies these semantics.

The accepted `@log` design calls for a fixed 4 KiB per-test buffer synthesized
in the driver. The current lowering emits framed stdout blocks instead. The
runner captures complete child output, then filters logs by test, suppresses
passing-test logs unless verbose, and caps displayed log bytes at 4 KiB per
test. This cap does not bound memory used by child-output capture.

When creating a copied test project for the CLI integration suite, give it a
`ZithProject.toml` with `test_dir = "tests"`. `findProjectRoot()` walks up from
the supplied directory, so without a local manifest a temporary project under
the repository can inherit the repository's manifest and discover the wrong
test directory.

The older `awt/agent1`..`awt/agent9` work and the hermes1 frontend commit are
already ancestors of `main`; every `awt/*` branch has zero commits ahead.
Hermes2 has no work to merge. The active worktree edits for `c/fs` were
committed as `7595e541`.

The earlier 2026-10-26 note that `zithc test` only type-checked is historical.
The runner has since been implemented, and the focused CLI integration tests
exercise runtime pass and failure reporting.

On 2026-10-27, the README pipeline and CLI tables were added to satisfy
`docs-consistency`, and the full CTest suite passed 46/46 after that change.
The full build passed with two `nodiscard` warnings in the untouched
`tests/test-telemetry.cpp`. `test-zith-test-suite` compares complete native and
VM v2 output and covers portable HIR behavior. The local `fmt-check` still
reports formatting violations across many files: this environment has
clang-format 23.1.1 and no clang-format-18, while the repository's expected
formatter is version 18. The integration test file itself is formatted by the
available formatter. `cppcheck` reports no finding in that test file but emits
existing warnings from unrelated project sources; `clang-tidy` reports no
findings in the test file. ASan/UBSan could not configure because `/usr/bin/cc`
lacks `libasan` and `libubsan`; Valgrind is not installed, so runtime
instrumentation was unavailable.

## std/test float helper gotcha

`assertNear(actual, expected, tolerance)` binds each macro argument once,
converts all three values to `f64`, then checks absolute distance and rejects
negative tolerances. It does not require identical argument types. Its failure
message does not format float values, avoiding a dependency on backend-specific
float formatting.

## clang-format version gotcha

The repo is formatted with the CI's clang-format-18, but the local Homebrew
`clang-format` is 23.1.1. The two disagree on several baseline lines (for
example `ParserDepthGuard(const ParserDepthGuard &) = delete;`, the
`index_`/`parser_depth_` alignment block in `ast-lowerer.hpp`, and the two
`TokenKind::Dots` conditions in `frontend-expr-primary.cpp`). The baseline
`main` therefore already fails a local `clang-format --dry-run --Werror`, and
reformatting those lines with 23.1.1 makes the diff diverge from the repo
style. Do not reformat unrelated lines to satisfy the local binary; keep the
diff limited to the intended hunks. `fmt-check` is a local-only CMake target
and is not part of `.github/workflows/ci.yml`.
