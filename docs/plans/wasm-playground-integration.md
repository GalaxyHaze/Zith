# WASM Playground Integration Plan

Status: proposed

## Objective

Make the browser playground execute Zith programs through the VM v2 runtime,
load the canonical standard library through a versioned release artifact, and
report source diagnostics instead of exposing compiler WASM traps.

## Preconditions

- Repository root: `/home/diogo/Zith`
- Website repository: `/home/diogo/Zith-website`
- Emscripten SDK: `/home/diogo/emsdk`
- Local WASM build command: `/home/diogo/Zith/scripts/build-wasm.sh`
- Existing browser ABI implementation: `/home/diogo/Zith/src/wasm/playground.cpp`
- Existing flat HIR implementation: `/home/diogo/Zith/src/wasm/abi-hir.cpp`
- Existing release workflow: `/home/diogo/Zith/.github/workflows/build-artifact.yml`
- Existing website loader: `/home/diogo/Zith-website/js/playground.js`

Do not use `git reset --hard`, `git checkout --`, broad cleanup commands, or
revert unrelated worktree changes. Preserve the existing native compiler path
and the canonical `/home/diogo/Zith/stdlib/` sources.

## Decisions

1. `check` validates and reports diagnostics without executing.
2. `run` compiles and executes through VM v2, reporting stdout, stderr, and
   exit code.
3. `build` prepares compiler/HIR inputs for future cache integration; the first
   implementation has no persistent browser cache.
4. `puts` and `println` must produce the same observable output as the native
   portable runtime.
5. The WASM build must not maintain a hand-written alternative standard
   library.
6. The release workflow packages the canonical stdlib and publishes matching
   WASM/stdlib artifacts with version and compatibility metadata.
7. User source errors return diagnostics and status `1`; they must not escape
   as `unreachable` or `memory access out of bounds`.
8. Existing low-level HIR exports remain available even after
   `zith_run_source` becomes the high-level execute operation.

## Execution Steps

### 1. Reproduce and localize the `std/io/console` trap

Goal: identify the first failing boundary without changing behavior.

1. Build the current WASM artifact with
   `/home/diogo/Zith/scripts/build-wasm.sh /tmp/zith-wasm-baseline`.
2. Instantiate the module with the complete import set reported by
   `WebAssembly.Module.imports`.
3. Run four sources independently:
   - `fn main() {}`
   - `extern fn puts(msg: *char)` with a call from `main`
   - `from std/io/console` with an empty `main`
   - `from std/io/console` with `println("text")`
4. Record whether the failure occurs in `zith_compile_source`,
   `zith_emit_hir`, or `zith_execute_hir`.
5. Repeat the same module-source fixture using a native host session and the
   VM v2 test harness.
6. Add reduced virtual-source fixtures until the smallest failing module is
   known. Do not replace the stdlib implementation as part of this step.

Expected output: the failure is reproducible with a named export and a
minimal source fixture; native and WASM results are recorded separately.

Failure checks:

- If the fixture succeeds, inspect the JavaScript import implementation for
  invalid pointer/length handling and repeat with host-write tracing.
- If both native and WASM fail, stop the browser work and fix the frontend or
  stdlib contract first.
- If only WASM fails, continue to the runtime/source-pack boundary.

Success criteria: a deterministic regression test can distinguish the failing
boundary and no conclusion is based only on the generic WASM trap text.

### 2. Replace the hand-written virtual stdlib path with a generated pack

Goal: the WASM runtime consumes the canonical `stdlib/` contents without
duplicating `std/io/console.zithm`.

1. Define a versioned pack format containing:
   - format version;
   - compiler/WASM ABI version;
   - stdlib version or Git release tag;
   - whole-pack SHA-256;
   - module path, byte offset/length, and checksum for each module.
2. Add a deterministic pack generator under `/home/diogo/Zith/scripts/`.
3. Generate both:
   - a development directory/manifest for inspection;
   - one release pack for low-request browser loading.
4. Add a WASM ABI entry point that accepts the pack bytes before compilation.
5. Validate pack version, ABI version, bounds, duplicate paths, and checksums
   before registering source modules.
6. Remove `kWasmConsoleModule` from the production path once the pack path is
   proven.
7. Keep native builds using `/home/diogo/Zith/stdlib/` directly.

Expected output: the same canonical `stdlib/std/io/console.zithm` is used by
native and WASM tests, and the browser artifact has no manually maintained
console source string.

Failure checks:

- If pack validation fails, return a stable invalid-pack diagnostic before
  parsing user source.
- If a module is absent, report its canonical path and pack version.
- If pack generation is nondeterministic, compare sorted file lists and
  SHA-256 output; do not publish it.

Success criteria: changing one canonical stdlib file changes the pack hash and
the WASM integration test observes the changed module.

### 3. Harden compiler diagnostics across the WASM boundary

Goal: all user-source lexer/parser/sema/lowering failures become status `1`
with readable diagnostics.

1. Add fixtures for incomplete delimiters, incomplete declarations, unknown
   imports, and invalid calls.
2. Run each fixture through `zith_compile_source`, `zith_emit_hir`, and the
   high-level run operation.
3. Ensure `last_error`, `error_count`, and `error_at` are reset at the start of
   every public operation.
4. Ensure no diagnostic path reads a stale pointer after a session is destroyed.
5. Ensure JavaScript catches unexpected WASM exceptions and labels them as
   runtime/compiler faults rather than pretending they are source diagnostics.
6. Keep status `2` for invalid ABI parameters, status `3`/`4` for VM runtime
   failures, and status `5` for unsupported HIR.

Expected output: malformed source prints a compiler diagnostic and returns
`1`; it never produces an uncaught `unreachable` or out-of-bounds trap.

Failure checks:

- If a fixture traps, reduce it to the smallest source and stop the feature
  implementation until the trap is fixed.
- If status is nonzero but `last_error` is empty, fix the ABI error surface
  before changing the website UI.
- If diagnostics from a previous run remain, add a state-reset assertion.

Success criteria: the complete malformed-source fixture table exits normally
and contains no uncaught WebAssembly exception.

### 4. Make `zith_run_source` execute through VM v2

Goal: one high-level WASM call compiles and executes a source program.

1. Change `/home/diogo/Zith/src/wasm/playground.cpp` so
   `zith_run_source(ptr, len)` calls the existing HIR encode/decode/VM path.
2. Preserve `zith_emit_hir` and `zith_execute_hir` as separate exports.
3. Reset source, diagnostic, output, HIR, and exit-code state before each call.
4. Forward VM stdout through `zith.host_write` stream `1`.
5. Forward compiler and VM errors through stream `2`.
6. Expose the final guest exit code through `zith_exit_code`.
7. Return status `5` for unsupported VM constructs without trapping.

Expected output: `extern fn puts` and `from std/io/console` programs execute
and report their output and exit code.

Failure checks:

- If HIR encoding fails, return status `1` with the encoder message.
- If HIR lowering returns unsupported, return status `5`.
- If VM returns a trap or out-of-memory, return status `3` or `4`.
- If output is duplicated, trace both `host_write` and buffered output and
  keep exactly one ownership path.

Success criteria: `zith_run_source` alone passes the puts/println execution
fixtures with expected output and exit code.

### 5. Update the website runtime flow

Goal: the website exposes `check`, `run`, and `build` with their agreed
semantics.

1. Update `/home/diogo/Zith-website/js/playground.js` to require the new ABI
   exports and pack metadata.
2. Load the versioned stdlib pack before accepting compiler commands.
3. Make `check` call the compile/check operation only.
4. Make `run` call `zith_run_source` or the explicit emit/execute sequence
   selected by the final ABI design; do not call a compile-only alias.
5. Render stdout and stderr separately.
6. Render `[program exited with code N]` after execution.
7. Make `build` report preparation success without claiming program execution.
8. Clear runtime state in the UI before every command.
9. Report ABI/version/pack mismatch as a load failure before compilation.

Expected output: the browser editor can check and run puts/println examples
and displays output plus exit code.

Failure checks:

- If the old WASM is loaded, fail with an explicit missing-export/version
  message.
- If the pack fetch fails, identify the URL and release version.
- If the command completes without output, inspect whether the VM path was
  called and whether `host_write` received stream `1`.

Success criteria: a local HTTP server serves the page, matching WASM, and
matching stdlib pack; a browser smoke test completes check and run.

### 6. Version and publish matching release artifacts

Goal: `build-artifact` publishes mutually compatible WASM and stdlib assets.

1. Update `/home/diogo/Zith/.github/workflows/build-artifact.yml` to generate
   the stdlib pack from the checked-in `stdlib/` tree.
2. Include release tag, ABI version, stdlib hash, and WASM hash in metadata.
3. Upload the versioned WASM ZIP and stdlib pack/ZIP in the same release.
4. Update the website workflow or update hook to copy both assets into the
   `Zith-website` playground.
5. Verify the website manifest points to the same release tag and hashes.
6. Fail the workflow if the WASM and pack metadata do not match.

Expected output: one release contains matching compiler WASM and stdlib
artifacts, and the website update consumes them together.

Failure checks:

- If a release contains only one of the pair, fail the workflow.
- If hashes differ, do not update the website.
- If an older release is selected, report the selected tag and stop.

Success criteria: a clean release workflow run produces all required assets
and the website loader accepts only the matching pair.

### 7. Add regression coverage and documentation

Goal: the behavior is enforced and documented at the same boundary where it
is consumed.

1. Update `/home/diogo/Zith/tests/test-wasm-runtime.mjs` for the complete
   current import set and the new high-level run semantics.
2. Add tests for puts, println, diagnostics, unsupported HIR, state reset,
   pack validation, and exit code.
3. Run host VM v2 tests and the WASM Node harness against the generated
   artifact, not a stale checked-in binary.
4. Update `/home/diogo/Zith/docs/wasm-playground-abi.md` with final exports,
   pack contract, status codes, and run/build semantics.
5. Update `/home/diogo/Zith/docs/adr/0024-wasm-vm-v2-abi.md` when the final
   implementation differs from this decision record.
6. Keep `/home/diogo/Zith/CONTEXT.md` limited to canonical domain terms.
7. Update website-facing documentation only for observable command behavior.

Expected output: focused CTest/Node tests pass, the ABI document matches the
exports, and the ADR/glossary no longer claim that run is compile-only.

Failure checks:

- If the test uses an export absent from the built artifact, fix the build or
  test contract; do not weaken the test.
- If host tests pass but WASM tests fail, treat it as a WASM regression.
- If docs disagree with the ABI, update the docs before merging.

Success criteria: all focused tests and the final website smoke test pass from
a clean generated artifact.

## Final Acceptance Check

Run these commands from the stated repositories after implementation:

```bash
cd /home/diogo/Zith
cmake --build build -j4
ctest --test-dir build --output-on-failure
./scripts/build-wasm.sh /tmp/zith-wasm-final
node tests/test-wasm-runtime.mjs /tmp/zith-wasm-final/zith-playground.wasm

cd /home/diogo/Zith-website
node --check js/playground.js
python3 -m http.server 4173
```

Then verify in a browser that `/playground/` loads the matching WASM/stdlib
assets, `check` reports diagnostics, `run` prints `puts` and `println`, and
the terminal displays the program exit code. Stop the temporary server after
the smoke test. Do not claim completion if any source fixture traps.
