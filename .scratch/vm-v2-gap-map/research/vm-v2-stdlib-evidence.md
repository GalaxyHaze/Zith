# VMG-03: Stdlib Parity Evidence

Baseline: `0c1ae39f7aa2f56e8e360bfd9de4a9d3a9aa21ca`
Research date: 2026-10-17
Scope: host-native versus host VM v2 for native-accepted stdlib programs. No
implementation or specification changes were made.

## Finding

The current parity run confirms seven stdlib-dependent programs that succeed
through native `zithc run` but diverge through VM v2 with status 5. The
harness allows these divergences. It does not establish runtime output
mismatches, because none of the VM results matches the native result. The
three console-literal cases are therefore not passing parity cases, despite
their placement in the documentation's “Passing programs” table.

The supported cases do not rely on a user-declared external FFI. They use
stdlib APIs and the compiler's common C/runtime surface. The language spec says
that `c/*` operations used by stdlib can resolve through native libc, VM
intrinsics, or WASM runtime imports, and distinguishes platform-specific
external headers separately (`docs/Zith--.md:69-86`). If “external FFI” is
intended to exclude even stdlib calls to common C/runtime functions, that
boundary would change which of these examples belong in the inventory.

## Confirmed Unsupported Cases

`ctest --test-dir build -V -R '^test-stdlib-parity$'` passed on the baseline,
but printed `ALLOW ... (status 5)` for each of these seven cases:

| Case | Native evidence | VM evidence | Classification and confidence |
|---|---|---|---|
| `console_println_literal` | `println(heap, "hello parity")`, case source at `tests/test-stdlib-parity.cpp:49-59` | status 5, allow-listed with a dyn-allocator reason | VM rejection is confirmed. The exact failing phase and root cause are unknown. |
| `console_print_no_newline` | Two `print(heap, ...)` calls at `tests/test-stdlib-parity.cpp:60-71` | status 5, same dyn-allocator reason | VM rejection is confirmed. The exact failing phase and root cause are unknown. |
| `console_println_two_literals` | `println(heap, "a", "b")` at `tests/test-stdlib-parity.cpp:72-82` | status 5, same dyn-allocator reason | VM rejection is confirmed. The native stdlib ignores surplus values, but VM parity is not reached. |
| `console_println_format_int` | `println(heap, "bucket=#", 73)` at `tests/test-stdlib-parity.cpp:83-92` | status 5, allow-listed as variadic `Formatable` unsupported | VM rejection is confirmed, but neither the phase nor the allow-list reason is verified as the actual cause. See the fast-path signature mismatch below. |
| `hash_map_u64_put_get` | `reserve`, `put`, `contains`, `get`, and `destroy` at `tests/test-stdlib-parity.cpp:97-121` | status 5, allow-listed as an unsupported HIR branch shape | VM rejection is confirmed. The harness does not retain the actual phase or lowering message, so the branch-shape attribution is unverified. |
| `allocator_heap_dyn_dispatch` | `allocate`/`deallocate` through `HeapAllocator` at `tests/test-stdlib-parity.cpp:122-134` | status 5, allow-listed as unsupported dyn allocator dispatch | VM rejection is confirmed. The reason is plausible from the stdlib API (`allocator.zithm:24-37`), but the harness does not prove the phase or failing operation. |
| `inplace_opaque_contract` | `Box as InPlace` with `args: opaque` at `tests/test-stdlib-parity.cpp:135-156` | status 5, allow-listed as unsupported opaque contract | VM rejection is confirmed. The harness does not expose the phase, actual failing HIR node, or diagnostic. |

The parity harness maps failures at three different stages to the same
unsupported status: failure to reach `HirLowered`, failure in `vm::lowerModule`,
and `Vm::runMain` returning `RunStatus::Unsupported`
(`tests/test-stdlib-parity.cpp:233-264`).
For allowed status 5 it prints the static allow-list reason, not
`Observed.detail` (`tests/test-stdlib-parity.cpp:303-314`). Consequently, the
individual phase and diagnostic cannot be recovered from the passing CTest
result alone. The table below therefore reports confirmed status-5 VM
rejections, not seven independently diagnosed lowering failures.

One likely stale integration seam explains the console cases, but is not
reported directly by the harness. The current stdlib declares
`print(allocator, msg, values)` and `println(allocator, msg, values)`
(`stdlib/std/io/console.zithm:275-284`). The VM intrinsic path identifies the
first call argument as the message slice (`src/vm/hir-to-vm.cpp:584-605`).
The parity programs pass the allocator first. The test-only virtual stdlib
uses the older `println(msg, values)` signature and passes
(`tests/test-vm-v2.cpp:890-967`), so that synthetic success does not prove
compatibility with the current stdlib API.

The formatted-output guard is implemented in VM lowering at
`src/vm/hir-to-vm.cpp:591-602`, but it inspects that same first argument.
Because `console_println_format_int` supplies the allocator first, source
inspection does not prove this case reaches the placeholder guard. Treat the
table's “variadic Formatable” reason as a description, not a verified root
cause. The current standard-library format contract defines `#` substitution,
escaped `\\#`, and missing values as `{absent}` (`docs/adr/0004-stdlib-io-format-contract.md:1-25`).

## Runtime Differences

No current runtime output or exit-code mismatch was confirmed in this
stdlib-parity slice. Every current VM case is unsupported, so execution does
not reach a comparable result. The parity document records an older silent
formatting mismatch where the VM printed the literal placeholder instead of
the formatted value, then says the current lowerer rejects that case instead
(`docs/vm-v2-stdlib-parity.md:59-66`). The current source includes the explicit
rejection (`src/vm/hir-to-vm.cpp:591-602`). This is historical evidence, not a
confirmed runtime mismatch at the baseline.

There is a separate test-suite signal. `ctest --test-dir build --output-on-failure
-R '^test-vm-v2$'` failed with 188 assertions passing and 11 failing. The real
stdlib console fixtures use the old no-allocator call form
(`tests/test-vm-v2.cpp:850-888`, `969-1010`), fail the modern HIR pipeline
assertion, and then fail their runtime/output assertions. The synthetic
old-signature fixture passes. This confirms that the real-stdlib integration
coverage is stale or broken, but it does not establish a runtime mismatch for
a valid current-API native program.

The input API is an important untested boundary, not a confirmed mismatch.
`std/io/console.input` reads via `getchar` (`stdlib/std/io/console.zithm:203-220`),
while the VM handler always returns end-of-input because the playground has
no stdin (`src/vm/vm-v2.cpp:264-271`). Native-versus-VM behavior for supplied
stdin is therefore likely different, but the current parity harness supplies
no stdin and has no input case. Do not classify this as a reproduced gap
without a controlled-input comparison.

## Accuracy And Coverage Of Existing Records

`docs/vm-v2-stdlib-parity.md:31-57` is inaccurate for the baseline in two
ways:

- It calls the three literal console cases passing, but the verbose current
  test run reports status 5 for all three.
- Its status-5 table lists four programs, while the test table has seven
  allow-listed cases. It omits the three console-literal programs.

The test's status-5 allowance is broader than its comments suggest. Every
entry in `kCases`, including all three cases labeled passing in the document,
has a non-null `allowStatus5Reason` (`tests/test-stdlib-parity.cpp:47-156`).
The harness accepts any native-success/VM-status-5 divergence when that
pointer is non-null (`tests/test-stdlib-parity.cpp:298-314`). Thus the current
“7 passed” result means there were no unallow-listed divergences. It does not
mean any of the seven cases matched, and the three purported passing cases
cannot currently detect a regression to unsupported status.

`docs/implementation-debt.md:214-223` accurately describes the intended
status-5 allow-list mechanism and the formatting safeguard. It overstates the
evidence when it says the list of programs that do not pass is documented in
the parity table, because that table omits three of the seven. It also labels
the debt “WASM vs native,” while this test compares host-native `zithc run`
with a direct host `vm::Vm` run (`tests/test-stdlib-parity.cpp:200-264`).
The host/VM gap is directly in scope; WASM-specific behavior is not assessed
here.

`docs/Zith--.md:77-86` describes the stdlib console API and the backend choices
for common C/runtime operations, but does not promise VM parity for these
features. `docs/adr/0004-stdlib-io-format-contract.md:1-25` specifies the
formatting and input contracts. No direct native-versus-spec discrepancy was
established in this investigation. The test failures indicate VM support or
test-fixture gaps, not evidence that native behavior violates the language
specification.

## Important Missing Examples

The parity slice contains only seven cases and no completed stdlib runtime
comparison. Useful native-accepted examples already exist in the repository:

- `examples/dyn-array.zithm:1-49` exercises generic `DynArray` reserve, growth,
  push, get, remove, pop, and destroy. It is registered as a native example
  with expected exit code 0 (`tests/test-examples.cpp:23-44`), but is absent
  from the VM parity case table.
- `examples/allocator-simple.zithm:1-32` exercises allocation, alignment,
  reallocation, preserved contents, and deallocation. It is registered as a
  native example (`tests/test-examples.cpp:23-25`), but parity currently has
  only one simple allocate/deallocate case.
- `stdlib/std/io/format.zithm:319-359` implements `Formatable` for slices,
  booleans, floats, integers, and C strings. The parity table only attempts an
  integer placeholder, and currently rejects before establishing the
  formatting behavior. Escaped placeholders, missing placeholders, extra
  values, no-placeholder messages, and the other supported formatter types
  have no VM/native comparison.
- `std/io/console.input` and its `ParseInput` implementations cover `i32`,
  `bool`, `f32`, `f64`, and `u32` (`stdlib/std/io/console.zithm:6-10`,
  `173-220`). There is no controlled-stdin parity example.
- The status-5 hash-map test only exercises the concrete `u64` map, while the
  repository also has a generic hash-map example (`examples/hash-map.zithm`).
  The generic target is explicitly documented as a draft due to generic
  reification and trait-bound limitations (`stdlib/std/collections/hash_map_u64.zithm:5-9`),
  so it should be classified separately from supported native `Zith--` API
  coverage rather than assumed to be a committed parity target.

`tests/test-examples.cpp` invokes `zithc run` without `--virtual-machine`
(`tests/test-examples.cpp:96-123`). Its passing examples establish native
coverage only, not VM parity.

## Reproduction And Limits

- Baseline commit checked: `0c1ae39f7aa2f56e8e360bfd9de4a9d3a9aa21ca`.
- `ctest --test-dir build -V -R '^test-stdlib-parity$'`: exit 0; all seven
  cases printed `ALLOW ... status 5`.
- `ctest --test-dir build --output-on-failure -R '^test-vm-v2$'`: exit 8;
  188 assertions passed and 11 failed, concentrated in real-stdlib console
  integration fixtures.
- OpenCodeRAG was not available in this session. The repository's local
  `python3 scripts/rag.py search ... --kind docs|code --json` fallback
  succeeded and was used before targeted source inspection.
- No code, tests, specifications, map, or ticket files were changed. This
  report is the only file created for VMG-03.
- The parity harness suppresses the actual VM detail for accepted status-5
  cases. Exact per-case failure stage and root cause remain unverified until
  the harness reports `Observed.detail` or runs stage-specific assertions.
