# WASM Playground VTables and Browser Command Chains

Status: implementation

## Objective

Make `dyn` interface dispatch execute through the browser WASM playground, let
the browser terminal run supported commands joined by `&&`, and synchronize
matching WASM, stdlib-pack, and manifest files for local verification without
publishing them.

## Preconditions

- Compiler repository: `/home/diogo/Zith`.
- Website repository: `/home/diogo/Zith-website`.
- Emscripten SDK: `/home/diogo/emsdk`.
- Reuse `/home/diogo/Zith/build` for native targets. Do not reconfigure or
delete this existing build tree.
- Generate the WASM build in the new directory `/tmp/zith-wasm-vtables`.
- Run every build and test command through `agent-run`.
- Limit builds and tests to four parallel jobs.
- Before writing to `/home/diogo/Zith-website`, request the filesystem
permission required for that checkout. Recheck its status first. Stop if any
target file has an unreviewed modification.
- The website checkout currently has unrelated user changes in
`.vscode/settings.json`, `README.md`, `notes/CONTEXT.md`,
`css/glarea-alternative.css`, `helios/alternative.html`, `media/Glarea/`,
`scripts/`, and `worker/`. Preserve them.
- The compiler checkout also has unrelated user changes. Do not reset, clean,
or revert them.
- The user approved Sequenta for implementation on 2026-11-21. The bounded
  implementation state machine lives in
  `docs/plans/wasm-playground-vtables/sequenta/`.

## Files In Scope

Compiler repository:

- `/home/diogo/Zith/src/wasm/abi-hir.cpp`
- `/home/diogo/Zith/tests/test-wasm-contract.cpp`
- `/home/diogo/Zith/tests/test-wasm-runtime.mjs`
- `/home/diogo/Zith/docs/wasm-playground-abi.md`
- `/home/diogo/Zith/docs/adr/0024-wasm-vm-v2-abi.md`
- `/home/diogo/Zith/CONTEXT.md`

Website repository:

- `/home/diogo/Zith-website/js/playground.js`
- `/home/diogo/Zith-website/js/playground-command-chain.js` (new)
- `/home/diogo/Zith-website/playground/index.html`
- `/home/diogo/Zith-website/tests/playground-command-chain.test.js` (new)
- `/home/diogo/Zith-website/playground/zith-playground.wasm`
- `/home/diogo/Zith-website/playground/zith-stdlib.pack`
- `/home/diogo/Zith-website/playground/runtime.json`

Do not edit `/home/diogo/Zith/.github/workflows/release-maintenance.yml`, do
not create a release, and do not push or deploy either repository in this
work.

## Verified Starting State

- The website manifest currently identifies its WASM runtime as `v0.6.4.2`.
- The checked-in WASM and stdlib-pack hashes match the checked-in manifest.
- Current local SHA-256 values:
  - `zith-playground.wasm`: `e74987fca88f5d307b29d8ebc020b992c3a00a4b322cdee17bed51d5aa4cffca`
  - `zith-stdlib.pack`: `1698d158842d8aac3e44c9f505e9b5c59bb343bb9ae4538b46ee14ada2d8fe98`
  - `runtime.json`: `95576653f8d035815caea79769c4061c2d2ac522bdb95bcdc783c5010cf5d57c`
- Running a dynamic-dispatch source through the website WASM returns
  unsupported status `5` with `missing HIR vtable in v2 lowering`.
- At baseline, `src/wasm/abi-hir.cpp` wrote `HirMakeDyn` and `HirDynCall`
  references but did not serialize `HirModule` vtable rows. Its type pool also
  stored `TypeStruct` IDs without the corresponding struct definitions. Both
  omissions must be covered for VM v2 dynamic dispatch over concrete structs.
- `/home/diogo/Zith/tests/test-vm-v2-gaps.cpp` already has
`test_dyn_method_dispatch`, which executes two implementations and expects
exit code `73`. Keep this regression test.
- The website terminal is implemented by `executeCommand` and
  `runCompiler` in `js/playground.js`. It accepts one command per input line.
  `clear` currently removes the command echo as well as previous output.
- The current browser's `run` action invokes `zith_run_source`. Passing
  `check` is not evidence that dynamic dispatch executes.
- The website checkout's current local files may differ from the live site.
A live-site read failure is not permission to publish or deploy.

## Execution Plan

### 1. Record the baseline and establish the red regression

Goal: preserve the current worktree state and have focused tests demonstrate
the missing vtable round-trip and browser execution before changing the codec.

Sub-steps:

### Phase A: record and verify the untouched baseline

1. Record `git status --short` in both repositories.
2. Record the current local hashes of the website WASM, stdlib pack, and
   manifest. Compare them with **Verified Starting State**.
3. Request the live manifest from `https://zith-lang.org/playground/runtime.json`.
   Record its response. If the request fails, record the curl error and
   continue with local verification only.
4. Build and run `test-wasm-contract` and `test-vm-v2-gaps` before changing
   tests. Both must pass against the unchanged source tree.

```bash
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith status --short
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith-website status --short
agent-run -t 30 -c 50 -- sha256sum /home/diogo/Zith-website/playground/zith-playground.wasm /home/diogo/Zith-website/playground/zith-stdlib.pack /home/diogo/Zith-website/playground/runtime.json
agent-run -t 30 -c 50 -- curl --fail --silent --show-error https://zith-lang.org/playground/runtime.json
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build --target test-wasm-contract test-vm-v2-gaps -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build -R '^test-wasm-contract$' --output-on-failure -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build -R '^test-vm-v2-gaps$' --output-on-failure -j4
```

Expected output: both worktree statuses are recorded, and the three local
hashes match **Verified Starting State**. The live manifest is recorded if
the request succeeds. If it fails, record the curl error and continue with
local verification only. Both focused CTests pass before test changes.

Failure checks:

- If a local hash differs, replace the recorded baseline with the observed
  value and re-check whether the local files still match their manifest.
- If either baseline CTest fails, record the failing test and assertion.
  Stop before adding regressions.
- If the live-manifest request fails, continue only with local verification.
  Do not treat that failure as permission to publish or deploy.

### Phase B: add and run the red regression

5. In `tests/test-wasm-contract.cpp`, add
   `test_hir_dyn_vtable_round_trip`. Construct two implementation functions,
   two `HirVTable` rows with module-local function IDs, and `HirMakeDyn` and
   `HirDynCall` expressions referring to those vtable names. Encode and decode
   the module. Assert that both vtable names, slot counts, slot function IDs,
   and dynamic-expression vtable references survive the round-trip.
6. Add a format-compatibility assertion to the same test file. Change the
   version word at byte offsets 4 through 7 of an encoded blob to version
   `6`. Assert that decoding rejects it after the implementation bumps the
   format version.
7. In `tests/test-wasm-runtime.mjs`, add a dynamic-dispatch source fixture
   with interface `Biased`, concrete implementations `First` and `Second`,
   and an `apply` function that calls through `dyn Biased`. Reuse the two
   cases from `test_dyn_method_dispatch`. Their combined result must be `73`.
8. Exercise the fixture through both `zith_emit_hir` plus
   `zith_execute_hir` and `zith_run_source`. Assert ABI status `0` and guest
   exit code `73` for both paths.
9. Build the focused native test targets and run the new regression against
   the current website WASM before implementing vtable serialization.

Commands:

```bash
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build --target test-wasm-contract test-vm-v2-gaps -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build -R '^test-vm-v2-gaps$' --output-on-failure -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build -R '^test-wasm-contract$' --output-on-failure -j4
agent-run -t 900 -c 50 -- node /home/diogo/Zith/tests/test-wasm-runtime.mjs /home/diogo/Zith-website/playground/zith-playground.wasm /home/diogo/Zith-website/playground/zith-stdlib.pack
```

Expected output: `test-vm-v2-gaps` passes with
`test_dyn_method_dispatch` returning exit code `73`. `test-wasm-contract`
fails because decoding loses the vtable rows. The Node fixture fails with
status `5` and `missing HIR vtable in v2 lowering`. Those two failures are
the expected red regression results before implementing serialization.

Failure checks:

- If the C++ test fails to compile because a HIR field or constructor differs,
  stop and use the definitions in `/home/diogo/Zith/src/hir/hir-module.hpp`
  and `/home/diogo/Zith/src/hir/hir-expr.hpp`. Keep the assertion intact.
- If the old WASM unexpectedly runs the new fixture successfully, record its
  manifest and hashes, then stop. Recheck the baseline diagnosis before
  implementation.

Success criteria: Phase A records both worktree states and all local hashes,
and both baseline CTests pass. In Phase B, the direct VM regression passes,
while the new round-trip and generated-runtime assertions fail specifically
because vtable rows are absent.

### 2. Version and implement flat-HIR dispatch metadata serialization

Goal: flat HIR version `7` faithfully round-trips the vtables and concrete
struct definitions needed by `HirMakeDyn` and `HirDynCall`, while rejecting
version `6`.

Sub-steps:

1. In `/home/diogo/Zith/src/wasm/abi-hir.cpp`, change `kBlobVersion` from
`6U` to `7U` and add version-history comments stating that version 7 adds
vtable rows and concrete struct-definition metadata.
2. Add a vtable writer beside `writeFunctions`. Write the vtable count, each
vtable name ID, the slot count, and each slot's module-local HIR function ID.
Use `kMaxArrayCount` as the count bound in the decoder.
3. Add explicit section marker `6` after the safety-proof section for
vtable rows. Preserve existing markers `1` through `5` and their order.
4. Add explicit section marker `7` after vtables for struct definitions.
Preserve each definition's name, ordered fields and field types, generic type
arguments, defining module, explicit alignment, and validated foreign layout.
5. Add matching decoders after safety-proof rows. Rebuild vtable rows through
`HirModule::addVTable`; rebuild struct definitions through `TypeIntern` while
preserving positional type IDs. Bound every new row and nested count, and set
useful `DecodedHir::message` values for invalid markers, oversized counts,
invalid rows, and truncation.
6. Leave the existing expression serialization for `HirMakeDyn` and
`HirDynCall` intact unless the new regression demonstrates that their
references also need correction.
7. Run the focused round-trip, malformed-row, and compatibility tests.

Commands:

```bash
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build --target test-wasm-contract -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build -R '^test-wasm-contract$' --output-on-failure
```

Expected output: `test-wasm-contract` passes. Decoded vtables retain exact
names and ordered slot IDs, and decoded concrete structs retain their names
and field layouts. A version-6 blob fails with `invalid flat HIR header`.

Failure checks:

- If a truncated or oversized vtable row is accepted, add a decoder guard
before proceeding.
- If an existing flat-HIR test fails, preserve the failing blob and assertion,
then correct the section order or field widths. Do not accept a version-7
writer paired with a different decoder layout.
- If vtable rows decode but concrete field reads change, inspect the decoded
  `TypeIntern` struct definitions before changing VM lowering.
- If both vtables and struct definitions round-trip but dynamic expressions
  still fail to lower, inspect `lowerMakeDyn` and `lowerDynCall` in
  `/home/diogo/Zith/src/vm/hir-to-vm.cpp`. Change VM lowering only if the
  round-tripped HIR proves the codec supplies the wrong data.

Success criteria: the focused CTest exits `0`, and version `6` is rejected
while version `7` round-trips the full vtable fixture.

### 3. Verify dynamic dispatch in native and generated WASM runtimes

Goal: both flat-HIR execution and the high-level browser WASM operation run
the two-implementation dynamic-dispatch fixture successfully.

Sub-steps:

1. Build `test-wasm-contract`, `test-wasm-playground`, and
`test-vm-v2-gaps` using the existing native build tree.
2. Run all three focused tests. Keep `test_dyn_method_dispatch` unchanged.
3. Build the WASM artifact in `/tmp/zith-wasm-vtables`. The repository build
script uses `nproc`. Export a Bash `nproc` function returning `4` so its
internal CMake build respects the four-job limit.
4. Run `test-wasm-runtime.mjs` against the newly generated WASM and pack, not
the checked-in website artifact.
5. Confirm the test reaches guest exit code `73` through both execution paths.

Commands:

```bash
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build --target test-wasm-contract test-wasm-playground test-vm-v2-gaps -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build -R '^(test-wasm-contract|test-wasm-playground|test-vm-v2-gaps)$' --output-on-failure -j4
agent-run -t 900 -c 0 -- bash -c 'nproc() { printf "4\n"; }; export -f nproc; EMSDK_DIR=/home/diogo/emsdk /home/diogo/Zith/scripts/build-wasm.sh /tmp/zith-wasm-vtables'
agent-run -t 900 -c 50 -- node /home/diogo/Zith/tests/test-wasm-runtime.mjs /tmp/zith-wasm-vtables/zith-playground.wasm /tmp/zith-wasm-vtables/zith-stdlib.pack
```

Expected output: all three focused CTests pass. The WASM build produces
non-empty `zith-playground.wasm` and `zith-stdlib.pack`. The Node harness
exits `0` and both dynamic-dispatch assertions report status `0` and guest
exit code `73`.

Failure checks:

- If the Node harness reports status `5`, inspect its error text. If it still
says `missing HIR vtable in v2 lowering`, the codec fix is incomplete.
- If the WASM build cannot find Emscripten, stop and report the configured
`EMSDK_DIR`. Do not switch to a native binary and call it a WASM test.
- If the test passes only through `zith_execute_hir` but not `zith_run_source`,
treat the browser runtime path as failing.

Success criteria: the generated-artifact Node test exits `0` and asserts
status `0` plus guest exit code `73` on both paths.

### 4. Add `&&` chaining to the browser terminal

Goal: the browser terminal runs supported commands in order, stops after the
first failure, and supports `clear && run` without adding general shell
features.

Sub-steps:

1. Add `/home/diogo/Zith-website/js/playground-command-chain.js` as a
DOM-free helper. Expose `parseCommandChain(line)` and
`runCommandChain(commands, executeOne)` to both the browser global and
CommonJS tests.
2. Make `parseCommandChain` split only on literal `&&`, trim each segment,
and reject an empty segment before executing any command. Do not parse
quotes, `;`, pipes, redirection, `||`, substitutions, or shell expansions.
3. Make `runCommandChain` call the dispatcher sequentially and stop when a
command returns `false`.
4. Load the helper before `playground.js` in
`/home/diogo/Zith-website/playground/index.html`.
5. Refactor `executeCommand` in `js/playground.js` to echo the full input
once, parse the chain, and execute its segments. Keep `clear` behavior:
clearing removes the echo and prior output, and `run` output appears after
the clear without re-echoing each segment.
6. Refactor the single-command dispatcher and `runCompiler` to return
success/failure. Treat compiler errors, invalid arguments, unavailable
runtime, runtime traps, and nonzero guest exit codes as failure. Treat
successful `clear`, `help`, `fmt`, `check`, and `build` operations as
success.
7. Ensure a nonzero `run` displays an error-styled exit status and returns
failure to the chain dispatcher.
8. Update the terminal `help` output to document `&&` short-circuit
behavior.
9. Add `/home/diogo/Zith-website/tests/playground-command-chain.test.js`
using Node's built-in `node:test` and `node:assert/strict`. Cover
`clear && run`, whitespace trimming, empty leading and trailing segments,
left-to-right order, and skipping the next command after a `false` result.

Commands:

```bash
agent-run -t 30 -c 50 -- node --check /home/diogo/Zith-website/js/playground-command-chain.js
agent-run -t 30 -c 50 -- node --check /home/diogo/Zith-website/js/playground.js
agent-run -t 30 -c 50 -- node --test /home/diogo/Zith-website/tests/playground-command-chain.test.js
```

Expected output: both syntax checks exit `0`. The Node test reports passing
cases for parsing, order, malformed chains, and short-circuit behavior.

Failure checks:

- If `&&` is treated as a compiler argument, the old single-command
dispatcher is still receiving the whole chain. Route each parsed segment to
that dispatcher instead.
- If `clear && run` leaves old terminal output, execute `clear` before
`run` and preserve the current clear operation that empties `output.textContent`.
- If a nonzero guest exit allows the following command to run, make
`runCompiler` return `false` for every nonzero guest exit.
- If an empty segment executes `clear`, validate the entire chain before
dispatching the first segment.

Success criteria: the Node command-chain tests pass, and
`clear && run` executes the current editor buffer after clearing previous
terminal output.

### 5. Update the ABI, decision, and domain documentation

Goal: the documented flat-HIR contract and playground command behavior match
the implemented version and supported dynamic-dispatch slice.

Sub-steps:

1. Update `/home/diogo/Zith/docs/wasm-playground-abi.md` to document flat
HIR version `7`, the vtable section, `dyn` dispatch support, and rejection
of older flat-HIR blobs. Keep the exported WASM ABI version at `2`.
2. Update `/home/diogo/Zith/docs/adr/0024-wasm-vm-v2-abi.md` to record
that the self-contained flat-HIR artifact includes vtables and that its
internal format version changed to `7`. Do not create a new ADR because
this extends the existing ABI decision.
3. Update the `WASM HIR flat` entry in `/home/diogo/Zith/CONTEXT.md` to
include vtable rows and their module-local function slots.
4. Keep the documented support boundary limited to constructs verified by
the VM v2 and WASM tests. Do not claim full LLVM parity.
5. Run documentation consistency checks and `git diff --check`.

Commands:

```bash
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build -R '^(docs-consistency|concepts-consistency|deltas-consistency)$' --output-on-failure -j4
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith diff --check
```

Expected output: documentation consistency tests pass, `git diff --check`
prints no errors, and the ABI document, ADR, and glossary describe the same
version and support boundary.

Failure checks:

- If the docs claim general trait/interface support beyond the tested
`dyn` vtable path, narrow the wording.
- If the ADR's ABI version is changed from `2`, restore it. Only the flat-HIR
blob version changes in this task.
- If consistency tests fail on unrelated pre-existing edits, record the
failing test and assertion. Do not revert the user's changes.

Success criteria: all documentation consistency tests exit `0`, and the
three documentation files agree on flat-HIR version `7`.

### 6. Synchronize matching local website artifacts

Goal: the website checkout contains the generated WASM, canonical stdlib
pack, and a manifest whose hashes identify those exact local files.

Sub-steps:

1. Before any website write, obtain the required filesystem approval.
2. Re-run `git status --short` in `/home/diogo/Zith-website`.
3. Confirm that `js/playground.js`, `js/playground-command-chain.js`,
`playground/index.html`, `playground/runtime.json`,
`playground/zith-playground.wasm`, and `playground/zith-stdlib.pack` have
no unreviewed changes. If any target is dirty, stop and ask the user how to
preserve it.
4. Copy the two generated artifacts from `/tmp/zith-wasm-vtables` into
`/home/diogo/Zith-website/playground/`.
5. Write `runtime.json` with version `local-worktree`, ABI `2`, stdlib name
`zith-stdlib.pack`, and SHA-256 values computed from the copied local
artifacts. Do not put a release tag in this local manifest.
6. Recompute and compare both hashes. Parse the JSON and assert the ABI and
asset name.
7. Do not modify or invoke the release-maintenance workflow.

Commands:

```bash
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith-website status --short
agent-run -t 30 -c 50 -- cp /tmp/zith-wasm-vtables/zith-playground.wasm /home/diogo/Zith-website/playground/zith-playground.wasm
agent-run -t 30 -c 50 -- cp /tmp/zith-wasm-vtables/zith-stdlib.pack /home/diogo/Zith-website/playground/zith-stdlib.pack
agent-run -t 30 -c 50 -- bash -c 'set -euo pipefail; SITE=/home/diogo/Zith-website/playground; WASM_SHA=$(sha256sum "$SITE/zith-playground.wasm" | cut -d" " -f1); STDLIB_SHA=$(sha256sum "$SITE/zith-stdlib.pack" | cut -d" " -f1); printf "{\"version\":\"local-worktree\",\"abi\":2,\"stdlib\":\"zith-stdlib.pack\",\"wasm_sha256\":\"%s\",\"stdlib_sha256\":\"%s\"}\n" "$WASM_SHA" "$STDLIB_SHA" > "$SITE/runtime.json"'
agent-run -t 30 -c 50 -- python3 -c 'import hashlib,json,pathlib; p=pathlib.Path("/home/diogo/Zith-website/playground"); m=json.loads((p/"runtime.json").read_text()); assert m["abi"]==2 and m["stdlib"]=="zith-stdlib.pack"; assert m["wasm_sha256"]==hashlib.sha256((p/"zith-playground.wasm").read_bytes()).hexdigest(); assert m["stdlib_sha256"]==hashlib.sha256((p/"zith-stdlib.pack").read_bytes()).hexdigest(); print("local playground manifest hashes match")'
```

Expected output: the final validation prints `local playground manifest
hashes match`. Both copied artifacts are non-empty and the manifest uses
`local-worktree`.

Failure checks:

- If filesystem approval is denied, stop before the first `cp`. Keep the
generated files in `/tmp/zith-wasm-vtables` and report that local website
synchronization remains unverified.
- If a copied file's hash does not match the manifest, regenerate the
manifest from the copied website files. Do not hand-edit a hash.
- If the website checkout changes during the copy, stop and report the
changed paths. Do not reset, clean, or overwrite them.

Success criteria: the manifest parses, ABI is `2`, asset name is exact, and
both SHA-256 values equal the copied files.

### 7. Run website tests and verify the playground locally

Goal: the built local website loads the matching artifacts and visibly
executes `clear && run` and dynamic dispatch without publishing anything.

Sub-steps:

1. Run syntax checks for `playground.js` and
`playground-command-chain.js`.
2. Run the new command-chain test and the existing website routing and editor
tests.
3. Build the static website with the documented Make target.
4. Start the local website server in a dedicated terminal, then open
`http://127.0.0.1:5501/playground/`.
5. With the default `Hello, World!` source, enter `clear && run`. Verify
previous output and the command echo disappear, program output appears, and
the terminal reports guest exit code `0`.
6. Replace the source with the `Biased` interface fixture and verify `run`
reports guest exit code `73`.
7. Enter `run && clear` with a source returning `73`. Verify `clear` does
not run after the nonzero result and the run failure remains visible.
8. Enter `clear &&` with existing terminal output. Verify the malformed
chain reports an error without executing `clear`.
9. Stop the local server with Ctrl-C.
10. Re-run hash validation and record the final `git status --short` for both
repositories. Do not stage or commit files.

Commands:

```bash
agent-run -t 30 -c 50 -- node --check /home/diogo/Zith-website/js/playground.js
agent-run -t 30 -c 50 -- node --check /home/diogo/Zith-website/js/playground-command-chain.js
agent-run -t 30 -c 50 -- node --test /home/diogo/Zith-website/tests/playground-command-chain.test.js
agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-routing.test.js
agent-run -t 30 -c 50 -- node /home/diogo/Zith-website/tests/docs-editor.test.js
agent-run -t 900 -c 0 -- make -C /home/diogo/Zith-website build
agent-run -t 0 -c 0 -- make -C /home/diogo/Zith-website serve
```

Expected output: syntax checks, Node tests, and the website build exit `0`.
The browser displays the correct output for each manual case. The local
manifest hashes still match the files served by the built site.

Failure checks:

- If the server port `5501` is occupied, stop and report the conflicting
process. Do not silently switch ports because the acceptance URL is fixed.
- If the browser reports a manifest/hash mismatch, inspect the served
`playground/runtime.json` and asset responses. Do not bypass loader validation.
- If `check` passes but `run` fails, treat execution as failing.
- If a browser is unavailable, report the automated results and mark the
manual browser acceptance as unverified.

Success criteria: all automated website checks pass, all four browser
acceptance cases behave as specified, and no release, push, or deployment
occurs.

## Final Acceptance Check

Run the complete native build and CTest suite once after the focused checks.
Then verify the generated WASM harness, website tests, browser cases, and
manifest hashes. The task is complete only when:

- `test-wasm-contract`, `test-wasm-playground`, and `test-vm-v2-gaps` pass.
- `test-wasm-runtime.mjs` passes against the newly generated WASM and pack,
including both dynamic-dispatch paths with guest exit code `73`.
- The full native CTest suite reports `100% tests passed`.
- The browser command-chain tests pass and local `clear && run` succeeds.
- The local website manifest identifies the exact synchronized WASM and
stdlib-pack hashes.
- Documentation consistently states flat-HIR version `7`, WASM ABI `2`,
and the tested `dyn` support boundary.
- No files were pushed, released, or deployed.

Commands:

```bash
agent-run -t 900 -c 0 -- cmake --build /home/diogo/Zith/build -j4
agent-run -t 900 -c 0 -- ctest --test-dir /home/diogo/Zith/build --output-on-failure -j4
agent-run -t 900 -c 50 -- node /home/diogo/Zith/tests/test-wasm-runtime.mjs /tmp/zith-wasm-vtables/zith-playground.wasm /tmp/zith-wasm-vtables/zith-stdlib.pack
agent-run -t 30 -c 50 -- node --test /home/diogo/Zith-website/tests/playground-command-chain.test.js
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith diff --check
agent-run -t 30 -c 50 -- git -C /home/diogo/Zith-website status --short
```

If any command or manual browser check fails, report the first failing output
verbatim and leave unrelated worktree changes untouched. Do not claim the live
site was updated. A separate explicit approval is required for release, push,
or deployment.
