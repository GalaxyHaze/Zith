# WASM Playground Interactive Input Plan

Status: signed, implementation in progress

Size: system

## Problem

Programs using `std/io/console.input()` cannot currently receive interactive
input from the browser playground terminal. The feature must add that path
without making the VM asynchronous or tying its I/O contract to WebAssembly.

## Non-Goals

- Do not add a WebAssembly-specific standard-library input API.
- Do not make the VM asynchronous or add Asyncify concepts to VM state.
- Do not add a worker, concurrent guest runs, or general computation
  preemption.
- Do not interrupt synchronous guest computation.
- Do not add unrelated terminal commands or change the flat HIR format.

## Open Decisions

None. The accepted contract is recorded in
[`docs/specs/wasm-playground-interactive-input.md`](../specs/wasm-playground-interactive-input.md).

## Preconditions

- Repository root: `/home/diogo/Zith`.
- Existing CMake build trees are `/home/diogo/Zith/build`,
  `/home/diogo/Zith/build-debug`, `/home/diogo/Zith/build-nollvm`, and
  `/home/diogo/Zith/build-novm`. Reuse `/home/diogo/Zith/build`; do not
  reconfigure or remove any existing build tree.
- Run every build, test, and probe through `agent-run`.
- Emscripten SDK is `/home/diogo/emsdk`. Use
  `/home/diogo/Zith/scripts/build-wasm.sh` for the browser artifact.
- The website repository is `/home/diogo/Zith-website`, outside the writable
  repository. Obtain access before editing it. If access is unavailable, stop
  before the website implementation and report the exact remaining files.
- Do not use Sequenta, add a worker, change the non-WASM execution model, or
  interrupt synchronous VM computation.
- Preserve all pre-existing worktree changes. Do not use `git reset --hard`,
  `git checkout --`, or broad cleanup commands.

## Accepted Contract

The normative requirements are in
[`docs/specs/wasm-playground-interactive-input.md`](../specs/wasm-playground-interactive-input.md).
The following promises identify the test seams accepted by
[`ADR-0063`](../adr/0063-wasm-playground-interactive-input.md).

1. Programs continue to call the existing `InputLine`/`input()` API.
2. While a WASM program waits for input, the browser terminal accepts one line.
   Enter delivers its UTF-8 bytes followed by `\n`, then resumes the program.
3. EOF is delivered separately from cancellation. If EOF follows bytes without
   a newline, `input()` returns that partial line once. Subsequent reads see
   EOF.
4. The VM exposes generic byte-oriented I/O callbacks. EOF is `-1`; cancellation
   is `-2` and maps to a dedicated VM run status. Asyncify is not represented in
   VM state or its run loop.
5. The terminal permits one active execution. Stop cancels a run only while it
   is suspended awaiting input. Synchronous guest computation runs on the
   browser thread and can block it until it returns.
6. `InputLine` grows its storage and has no fixed 256-byte line limit.
7. Increment the WASM execution ABI from 2 to 3. Preserve `zith.host_write`,
   and add the byte-input host import through Emscripten's generated JavaScript
   glue and the existing `instantiateWasm` integration point.
8. Keep the change to the VM I/O seam, the WASM build/ABI, the terminal input
   path, `InputLine`, and focused tests/documentation. Do not add a general
   asynchronous VM, background execution, multiple concurrent runs, or
   unrelated terminal features.

## Promises

### ABI-WASM-INPUT-01

```text
id: ABI-WASM-INPUT-01
contract: VM byte-oriented host I/O
rule: VM v2 delivers bytes through a generic read callback, treats -1 as EOF
and -2 as cancellation, reports cancellation with a distinct run status, and
streams output before input reads while preserving the accumulated output.
seam: tests/test-vm-v2.cpp::test_vm_v2_byte_io_callbacks
layout: none
fields: none
reverse: The VM contract becomes platform-specific or requires asynchronous
execution in the VM run loop.
```

### ABI-WASM-INPUT-02

```text
id: ABI-WASM-INPUT-02
contract: InputLine line and EOF behavior
rule: InputLine accepts lines longer than 256 bytes, omits the newline, and
returns bytes preceding EOF as one final line before reporting EOF.
seam: tests/test-vm-v2.cpp::test_vm_v2_stdlib_input_grows_and_returns_partial_eof_line
layout: none
fields: none
reverse: InputLine adopts an explicitly bounded line-size contract.
```

### ABI-WASM-INPUT-03

```text
id: ABI-WASM-INPUT-03
contract: WASM playground input ABI
rule: The execution ABI is version 3, preserves zith.host_write, maps host
input and cancellation to VM v2, and leaves flat HIR at version 7.
seam: tests/test-wasm-playground.cpp::test_wasm_playground_interactive_input
layout: none
fields: none
reverse: The playground no longer exposes VM v2 execution through this host
ABI.
```

### ABI-WASM-INPUT-04

```text
id: ABI-WASM-INPUT-04
contract: Async WASM host read
rule: Emscripten-generated Asyncify glue suspends a guest byte read until the
host supplies input, then resumes the guest execution.
seam: tests/test-wasm-runtime.mjs
layout: none
fields: none
reverse: The browser runtime no longer uses Emscripten-generated glue for
asynchronous host imports.
```

### ABI-WASM-INPUT-05

```text
id: ABI-WASM-INPUT-05
contract: Browser terminal input and cancellation
rule: The terminal submits UTF-8 input followed by newline while one guest run
is waiting. EOF ends input, and Stop cancels only a run suspended on input.
seam: Browser acceptance test for /playground/ in Zith-website
layout: none
fields: none
reverse: The playground no longer accepts guest input through its terminal.
```

## Execution Steps

### 1. Add the generic VM byte-I/O seam and regression coverage

Goal: `Vm::runMain` accepts generic byte-I/O callbacks, and
`tests/test-vm-v2.cpp` verifies byte reads, EOF, and cancellation without
depending on WebAssembly or Asyncify.

1. Update `/home/diogo/Zith/src/vm/vm-v2.hpp` to define the generic
   byte-oriented callback contract and the dedicated cancelled run status.
2. Update `/home/diogo/Zith/src/vm/vm-v2.cpp` to carry callbacks in the local
   run state and dispatch byte reads through the generic I/O boundary.
3. Map callback results `0..255` to bytes, `-1` to EOF, and `-2` to
   cancellation. Treat other negative values as I/O failures using the
   existing runtime-error path.
4. In `/home/diogo/Zith/tests/test-vm-v2.cpp`, add focused fixtures that
   execute a guest read through the existing extern-call path.
5. Verify ordinary bytes arrive in order, handled EOF leaves the run
   successful, and cancellation produces the distinct run status.
6. Keep existing no-callback behavior deterministic for programs that do not
   read input. Preserve the current output buffer result.

Command:

```bash
cd /home/diogo/Zith
agent-run -t 900 -c 0 -- cmake --build build -j4 --target test-vm-v2
agent-run -t 120 -c 50 -- ./build/tests/test-vm-v2
```

Expected output: the target builds and `test-vm-v2` exits with status `0`;
existing VM tests and new byte/EOF/cancellation checks pass.

Failure checks:

- If a fixture cannot express an input extern with the current VM test helpers,
  add only the smallest module fixture needed in `tests/test-vm-v2.cpp`.
- If a `src/vm/` change requires an Asyncify header, promise, or WASM macro,
  remove that dependency and keep suspension in the host integration.
- If adding the callback changes callers of `Vm::runMain`, update only the
  compiler-reported call sites and retain a default/no-input path.
- If existing VM output changes for programs without reads, preserve the
  previous no-input behavior.
- If the cancelled status is serialized into an unrelated ABI or cache format,
  stop and remove that expansion. Cancellation is a run result only.

Success criteria: both commands exit `0`, the tests distinguish EOF from
cancellation, and `src/vm/` contains no WebAssembly- or Asyncify-specific
types or conditionals for this feature.

### 2. Remove the fixed line-size limit from `InputLine`

Goal: `input()` reads complete lines longer than 256 bytes and handles EOF
after a partial line as specified.

1. In `/home/diogo/Zith/stdlib/std/io/console.zithm`, change `InputLine` from a
   fixed 256-byte allocation to storage that grows as bytes arrive, using the
   allocator already held by `InputLine`.
2. Update allocation and destruction sizes so they track the actual capacity.
3. Make newline return the accumulated line without including the newline.
4. Make EOF after accumulated bytes return that partial line once; make the
   next read report EOF with no line.
5. Preserve the existing empty-input and read-failure behavior when no bytes
   have been accumulated.
6. Add or extend the end-to-end VM test in
   `/home/diogo/Zith/tests/test-vm-v2.cpp` to cover a line longer than 256
   bytes, newline termination, and EOF after a partial line. Keep the input
   callback generic so the test does not depend on WebAssembly.

Commands:

```bash
cd /home/diogo/Zith
agent-run -t 900 -c 0 -- cmake --build build -j4 --target test-vm-v2
agent-run -t 120 -c 50 -- ./build/tests/test-vm-v2
```

Expected output: `test-vm-v2` exits `0`; the long-line case preserves every
byte and the partial line is returned exactly once.

Failure checks:

- If the allocator API cannot resize an allocation, use allocate-copy-release
  with an explicitly tracked capacity. Do not reintroduce a fixed maximum.
- If parsing helpers still assume a 256-byte line, change only the scratch
  buffer limits needed to preserve parsing of the complete line; do not
  truncate the stored input.
- If EOF-after-partial changes behavior outside `input()`, stop and confine the
  change to `InputLine`.

Success criteria: the standard-I/O test command exits `0`, and a line of at
least 1024 bytes round-trips without truncation.

### 3. Connect the WASM runtime to byte input and bump its ABI

Goal: the WASM runtime exposes ABI 3 byte input and passes it to VM v2 without
changing the existing stdout/stderr import.

1. In `/home/diogo/Zith/src/wasm/playground.cpp`, implement the VM input
   callback using the new `zith.host_read_byte` import.
2. Keep `zith.host_write` as the output callback and preserve stream IDs and
   existing compile/run status behavior.
3. Return a distinct WASM run result for VM cancellation. Do not report it as
   EOF, a source diagnostic, or a runtime trap.
4. Update the WASM ABI constant and any ABI 2 assertions in
   `/home/diogo/Zith/src/wasm/`, `/home/diogo/Zith/scripts/package-stdlib.py`,
   and `/home/diogo/Zith/docs/wasm-playground-abi.md` to ABI 3.
5. Update the release manifest emitted in
   `/home/diogo/Zith/.github/workflows/release-maintenance.yml` from ABI 2 to
   ABI 3.
6. Keep the flat HIR format version unchanged. It is independent of the
   execution ABI.
7. Update `/home/diogo/Zith/tests/test-wasm-playground.cpp` with host callback
   coverage for byte delivery, EOF, and cancellation.

Commands:

```bash
cd /home/diogo/Zith
agent-run -t 900 -c 0 -- cmake --build build -j4 --target test-wasm-playground
agent-run -t 120 -c 50 -- ./build/tests/test-wasm-playground
```

Expected output: the native WASM-playground test executable exits `0`; ABI 3
reports byte input while the existing output and compile paths continue to
pass.

Failure checks:

- Before updating any additional ABI 2 reference, verify it refers to the WASM
  execution ABI and not the flat HIR format or an unrelated format version.
- If `zith.host_write` changes signature or behavior, revert that unrelated
  change and keep output compatibility.
- If cancellation is exposed as a compile failure or runtime trap, stop and
  correct the public status mapping before website work.

Success criteria: the focused WASM playground tests pass, and this command
finds no remaining reference to the old execution ABI in these locations:

```bash
cd /home/diogo/Zith
agent-run -t 30 -c 50 -- rg -n 'ABI_VERSION = 2|abi":2|execution ABI version 2' scripts/package-stdlib.py src/wasm docs/wasm-playground-abi.md .github/workflows/release-maintenance.yml
```

### 4. Build the Asyncify artifact and test the generated glue

Goal: the browser artifact can suspend and resume through Emscripten-generated
JavaScript glue, without making the VM asynchronous.

1. Update `/home/diogo/Zith/CMakeLists.txt` and
   `/home/diogo/Zith/scripts/build-wasm.sh` so the playground WASM build
   enables Asyncify and emits the generated JavaScript glue required to handle
   asynchronous host imports.
2. Include `zith.host_read_byte` in the Asyncify import configuration.
3. Keep the generated JS and WASM outputs together in the build output
   directory. Do not hand-write an Asyncify state machine.
4. Update `/home/diogo/Zith/tests/test-wasm-runtime.mjs` to load the generated
   glue and provide an asynchronous `host_read_byte` implementation.
5. Test a guest that reads multiple bytes, a line ending in newline, EOF after
   partial bytes, and cancellation.
6. Build into a fresh temporary output directory, not an existing project
   build tree.

Commands:

```bash
cd /home/diogo/Zith
agent-run -t 900 -c 0 -- ./scripts/build-wasm.sh /tmp/zith-wasm-interactive-input
agent-run -t 120 -c 50 -- node tests/test-wasm-runtime.mjs /tmp/zith-wasm-interactive-input
```

Expected output: the build exits `0`, emits both the generated glue and
`zith-playground.wasm`, and the Node harness exits `0` after exercising
asynchronous input and resumption.

Failure checks:

- If the generated glue does not expose an `instantiateWasm` hook, stop and
  inspect the Emscripten output mode/options. Do not fall back to direct
  `WebAssembly.instantiate` for the Asyncify path.
- If the guest export returns before the host input promise resolves, correct
  the Asyncify import/export configuration and repeat the harness.
- If the Node harness requires a browser DOM, isolate the glue instantiation
  from the DOM code in the harness. Do not add a separate production runtime.

Success criteria: the WASM build and Node harness both exit `0`; tests prove
that the guest resumes after an asynchronous byte read.

### 5. Route the browser terminal into a suspended guest run

Goal: users can type input in the existing playground terminal and resume the
single active guest program.

1. Request authorized write access to `/home/diogo/Zith-website` before editing
   files outside the compiler repository.
2. Update `/home/diogo/Zith-website/js/playground.js` to load the generated
   Emscripten glue and inject the `host_read_byte` implementation through its
   existing `instantiateWasm` hook.
3. While a run awaits input, route the existing `#terminal-input` field to the
   guest instead of interpreting its text as a playground command.
4. On Enter, enqueue the field's UTF-8 bytes followed by newline and resume the
   suspended run.
5. Provide explicit EOF and Stop actions while input is pending. EOF resolves
   the pending read with `-1`; Stop resolves it with `-2`.
6. Disable a second Run while one run is active. Restore normal terminal
   command handling after the run finishes or is cancelled.
7. Update `/home/diogo/Zith-website/playground/index.html` only for the
   minimal EOF/Stop controls required by this contract.

Expected output: while the sample program waits in `input()`, the terminal
shows the waiting state, accepts one line on Enter, then displays subsequent
program output. EOF ends input without marking the run cancelled. Stop returns
the dedicated cancellation result.

Failure checks:

- If the terminal treats guest input as a command, ensure the active-run input
  state takes precedence and normal command handling resumes only after the
  run ends.
- If Stop is available during synchronous computation, disable or hide it
  until the guest is suspended for input.
- If website write access is denied, make no attempt to write outside the
  repository. Report the remaining website files and stop this step.

Success criteria: the browser manual test passes for line input, EOF,
cancellation, and terminal command behavior after completion, with at most
one guest run active.

### 6. Update the ABI documentation and run focused regression checks

Goal: the ABI document and all focused tests describe and verify the delivered
input contract.

1. Finish the ABI 3 import/status documentation in
   `/home/diogo/Zith/docs/wasm-playground-abi.md`.
2. Add the browser-terminal usage and limitations to the existing playground
   documentation in `/home/diogo/Zith-website/`, without duplicating the
   standard-library `InputLine` API documentation.
3. Build the focused native targets:

```bash
cd /home/diogo/Zith
agent-run -t 900 -c 0 -- cmake --build build -j4 --target test-vm-v2 test-stdlib-parity test-wasm-playground
```

4. Run each focused executable and the WASM Node harness:

```bash
cd /home/diogo/Zith
agent-run -t 120 -c 50 -- ./build/tests/test-vm-v2
agent-run -t 120 -c 50 -- ./build/tests/test-stdlib-parity
agent-run -t 120 -c 50 -- ./build/tests/test-wasm-playground
agent-run -t 120 -c 50 -- node tests/test-wasm-runtime.mjs /tmp/zith-wasm-interactive-input
```

5. Run `git diff --check` in `/home/diogo/Zith`.
6. Review `git status --short` and confirm all pre-existing modified and
   untracked files remain untouched.
7. Run the browser manual checks from Step 5 if the website repository is
   accessible.

Expected output: every build/test command exits `0`, `git diff --check` exits
`0`, and the manual browser check confirms line input, partial-line EOF,
cancellation, and the synchronous-computation limitation.

Failure checks:

- If any focused test fails, run only that failing test under ASan+UBSan using
  the project's existing sanitizer build configuration, then fix the defect
  before proceeding.
- If the Node harness passes but the browser check fails, treat browser glue
  and terminal state as unverified. Do not claim end-to-end success.
- If unrelated worktree paths changed during the task, preserve them and report
  them. Do not clean or revert them.

Success criteria: focused VM, stdlib, WASM-native, and Node tests pass;
documentation reports ABI 3; `git diff --check` is clean; and the manual
browser path passes or is explicitly reported as blocked by website access.
