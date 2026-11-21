# VMG-02: Runtime Parity Against Native

Status: researched against baseline `0c1ae39f7aa2f56e8e360bfd9de4a9d3a9aa21ca`
(`chore: checkpoint compiler and stdlib updates`). This report records evidence
only. It does not define behavior or propose implementation changes.

## Finding

No runtime mismatch is confirmed by the current cross-backend test evidence.
The only native-versus-VM test, `test-stdlib-parity`, ran all seven listed
programs through native successfully, while the VM path diverged with
unsupported status 5 for every one. The test maps HIR-stage failure and
`vm::lowerModule` failure to status 5 before execution, but VM execution can
also return unsupported. The passing CTest result therefore establishes
allow-listed divergences, not their exact phase or a runtime comparison.

Four of those seven programs exercise standard-library console and allocator
paths. They use the VM's `puts`/`write_stdout` host handlers or allocator
externs if lowered, so they touch the FFI/runtime boundary and are not treated
here as core, FFI-independent runtime parity evidence. The hash-map case also
uses allocator and validated extern paths. The remaining case,
`inplace_opaque_contract`, has no explicit external call in its source, but VM
lowering rejects it before it can provide a runtime comparison.

## Reproduction And Baseline Results

The parity harness writes each source from `kCases`, runs native `zithc run`,
lowers the same source to VM v2, and compares the results. Native failure fails
the test. VM status 5 passes only when the case has a reason in the table.
Sources and harness logic are in `tests/test-stdlib-parity.cpp:47-164` and
`:203-269`; the comparison loop is at `:273-324`.

Command:

```sh
ctest --test-dir build -R 'test-vm-v2|test-stdlib-parity' --output-on-failure
ctest --test-dir build -R test-stdlib-parity --output-on-failure -V
```

Observed on this checkout, whose `HEAD` is the stated baseline:

- `test-stdlib-parity`: 7 passed, 0 failed. Verbose output reports `ALLOW` with
  VM status 5 for all seven cases.
- `test-vm-v2`: 188 checks passed and 11 failed. The failed checks cluster in
  three older source-level console tests: their `println`/`print` calls omit
  the allocator argument now required by `stdlib/std/io/console.zithm:276-285`.
  These are compile/pipeline or lowering failures, not observed runtime
  mismatches. The suite's relevant old calls are at
  `tests/test-vm-v2.cpp:858-862`, `:977-986`, and `:1228-1235`.

The CTest parity command is reproducible at the committed baseline. It is a
small, stdlib-focused corpus and does not establish parity for all programs
accepted by both backends.

## Current Status-5 Divergences

These cases are accepted and run by native, but the VM path reports status 5.
The verbose test output prints the allow-list reason rather than the observed
`vmRun.detail`, so it does not establish whether each case failed in HIR
lowering, VM lowering, or VM execution. They are confirmed native/VM
divergences, but not confirmed runtime-result mismatches.

| Case | Native result | Current VM result | Evidence and likely dependency |
| --- | --- | --- | --- |
| `console_println_literal` | Exit 0, `hello parity\n` | Status 5; phase unknown | Allow-list label cites `dyn Allocator` parameter layout. Plausible dependency, but not verified as the observed cause. |
| `console_print_no_newline` | Exit 0, `ab` | Status 5; phase unknown | Same allow-list label and caveat. |
| `console_println_two_literals` | Exit 0, `a\n` | Status 5; phase unknown | Allow-list label cites allocator layout. The extra value is not established as the cause. |
| `console_println_format_int` | Exit 0, `bucket=73\n` | Status 5; phase unknown | Allow-list label cites variadic `Formatable`; the case also uses the allocator-taking console API, so the actual cause is unverified. |
| `hash_map_u64_put_get` | Exit 0, empty output | Status 5; phase unknown | Allow-list label cites an unsupported HIR branch shape. The case also exercises allocator and validated extern paths; the actual cause is unverified. |
| `allocator_heap_dyn_dispatch` | Exit 42, empty output | Status 5; phase unknown | Allow-list label cites dynamic `Allocator` dispatch. Plausible dependency, but not verified as the observed cause. |
| `inplace_opaque_contract` | Exit 42, empty output | Status 5; phase unknown | Allow-list label cites an opaque `InPlace` contract. The actual failing phase and operation are unverified. |

The case source and per-case reasons are in
`tests/test-stdlib-parity.cpp:49-164`. The verbose CTest output records all
seven status-5 outcomes but prints the static reason rather than the observed
detail. The harness maps HIR/lowering failure to unsupported before invoking
the VM at `tests/test-stdlib-parity.cpp:233-256`; it also maps a runtime
`RunStatus::Unsupported` to status 5 at `:258-264`.
The native results above were also reproduced directly for each persisted
source under `/tmp/zith-stdlib-parity-tests/<case>/main.zithm`, using
`/home/diogo/Zith/build/zithc --include /home/diogo/Zith/stdlib run <source>`.
All seven invocations had empty stderr. The two exit-42 results are the source
programs' intentional return values, not native compile failures.

### Documentation Drift

`docs/vm-v2-stdlib-parity.md:31-42` currently says the three literal console
cases pass through both backends. At baseline, the test instead reports status
5 for those cases, matching the console API's allocator parameter at
`stdlib/std/io/console.zithm:276-285`. The document's four other status-5 cases
at `:44-54` agree with the verbose test output. The three “passing programs”
entries should therefore be treated as stale evidence, not current parity.

The same document records an earlier runtime output divergence for formatted
printing: VM previously printed the literal `bucket=#`, while native printed
`bucket=73`. It says lowering was changed to reject that placeholder-plus-value
shape rather than silently drop values (`docs/vm-v2-stdlib-parity.md:58-67`,
`src/vm/hir-to-vm.cpp:590-602`). At this baseline this is a lowering rejection,
not a confirmed runtime mismatch. Calls with placeholders but no values and
other formatting forms are not established by this test.

## Runtime Evidence And Coverage Limits

The current parity corpus does not demonstrate a successful comparable VM
execution for any of its seven cases, so it confirms neither equality nor
inequality for runtime output, exit code, control flow, memory behavior, or
traps. It also cannot prove that every status-5 result occurred before
execution.

`tests/test-vm-v2.cpp` does provide VM-only expected-result checks:

- Branch loops and two-way branches: `:132-175`.
- Linear-memory reads, invalid writes, allocation lifetimes, and nested frames:
  `:380-620`.
- Direct/indirect calls and their invalid-reference traps: `:230-371`.
- Bitwise operations and shifts: `:1638-1676`.
- Lowered loop/control-flow plus array indexing, and negative/out-of-range
  index traps: `:1678-1773`.

These tests establish selected VM behavior but do not run the same source
through native, so they cannot identify parity. Likewise, the fixed
`int64_t`/bits representation is an explicit VM design limitation
(`docs/plans/vm-v2.md:82-85`, `docs/implementation-debt.md:211-213`), but this
audit found no paired native reproducer establishing a runtime mismatch from
it. Width-sensitive arithmetic, overflow, float behavior, and trap parity
remain unverified here. No discrepancy between native behavior and
`docs/Zith--.md` was established by the available paired executions.

## Harness Confidence Caveat

The parity helper defines equality using only exit code and stdout:
`tests/test-stdlib-parity.cpp:267-269`. It does not compare `Observed.status`.
The VM result defaults to status `Ok`, exit code `0`, and empty output
(`src/vm/vm-v2.hpp:19-24`). On a runtime trap, `Vm::runMain` sets the status
and message but returns before copying a program result into the exit code or
output (`src/vm/vm-v2.cpp:898-946`). Therefore a VM trap with empty output
could compare equal to a native program that exits zero without output.

This false-positive shape is not one of the current seven observed cases:
unsupported lowering leaves the harness `Observed.exitCode` at `-1`, and the
current corpus does not reach runtime traps. It is a test-coverage risk, not a
confirmed product mismatch. A future paired corpus should compare successful
VM status as well as stdout and exit code before claiming runtime parity.

## Search And Scope Notes

OpenCodeRAG status showed an indexed workspace, but semantic queries could not
connect to its Ollama service in this environment (`EPERM` on
`127.0.0.1:11434`). The repository's local RAG search fallback was used to
locate the parity harness, VM tests, and VM state/debt docs. Findings above
were then checked against the committed source and test files. The parity
harness marks any launched native process as status OK even if its program
returns nonzero (`tests/test-stdlib-parity.cpp:202-229`), so native acceptance
was confirmed separately by checking that all seven invocations produced no
diagnostic and returned the case results listed above.

External-FFI-dependent behavior and WASM/browser behavior were excluded from
the runtime conclusion. No implementation, test, spec, map, or ticket files
were changed as part of this research.
