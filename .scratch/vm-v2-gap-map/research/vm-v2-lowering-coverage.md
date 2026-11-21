# VMG-01: VM v2 Lowering Coverage Against Native

Baseline: `0c1ae39f7aa2f56e8e360bfd9de4a9d3a9aa21ca`

## Scope and method

This inventory compares the host-native LLVM path with the active VM v2 path.
It includes user-language constructs that reach HIR and are supported by native
codegen but have no corresponding VM lowering. It excludes WASM/browser behavior
and behavior that requires external C FFI. It does not propose implementation
changes or language specifications.

The local build is configured with `ZITH_HAS_LLVM=ON` and `ZITH_BUILD_VM=ON`
(`build/CMakeCache.txt:368,404,419`). In this configuration, `zithc run` without
`--virtual-machine` selects native codegen, while the explicit flag selects
`vm::lowerModule` (`src/cli/cmd/run.cpp:60-70`). The parity harness captures the
native CLI result and separately runs the same source through HIR lowering and
VM v2 (`tests/test-stdlib-parity.cpp:203-258`).

OpenCodeRAG was installed, but its `query` could not reach the local Ollama
service (`connect EPERM 127.0.0.1:11434`). I used the repository's daemon-free
RAG fallback, then checked every finding against source, tests, and
`docs/Zith--.md`.

## Findings

### Confirmed in-scope lowering gaps

| Area | Evidence that native accepts the behavior | VM lowering gap and evidence |
|---|---|---|
| Optional construction and extraction | Native tests execute optional conditions and `must` extraction (`tests/test-codegen.cpp:2701-2721,2795-2810`). HIR tests show checked array indexing and slicing materialize `HirMakeSome` and `HirMakeNone` (`tests/test-hir-lower-modern.cpp:1950-2025`). | `lowerOperand` has no handlers for `HirMakeSome` or `HirMakeNone`; the fallback reports `unsupported HIR expression in v2 lowering` (`src/vm/hir-to-vm.cpp:982-1302`). This blocks ordinary optional values and HIR paths that synthesize optionals, not just optional syntax by itself. |
| Checked optional panic | Native `must none` terminates through runtime error `R10003` (`tests/test-codegen.cpp:2813-2823`). HIR lowering creates `HirRuntimePanic` as the None-path terminator (`src/sema/hir-lower-expr-value.cpp:680-715`), and LLVM emits an unreachable trap (`src/codegen/codegen-emit-expr.cpp:982-1004`). | VM terminator lowering handles only `HirRet`, `HirJump`, and `HirBranch`, with no fallback rejection for other terminators (`src/vm/hir-to-vm.cpp:1593-1664`). `HirRuntimePanic` is therefore not represented and may be silently omitted instead of producing a status-5 diagnostic. Exact VM execution outcome is unverified. |
| Enum values | Native codegen executes an enum-to-integer cast and checks the discriminant result (`tests/test-codegen.cpp:562-579`). The LLVM emitter handles `HirEnumValue` (`src/codegen/codegen-emit-expr.cpp:121-124`). | `HirEnumValue` has no VM visitor handler and reaches the generic unsupported-expression failure (`src/vm/hir-to-vm.cpp:982-1302`). |
| Unions | HIR tests produce raw-union casts and tagged-union checks (`tests/test-hir-lower-modern.cpp:2147-2224`). Native codegen implements union casts/checks (`src/codegen/codegen-emit-expr.cpp:211-286`); its raw-union runtime test constructs a value and reads another member (`tests/test-codegen.cpp:2136-2150`). | `HirUnionCast` and `HirUnionCheck` have no VM visitor handlers and reach the generic unsupported-expression failure (`src/vm/hir-to-vm.cpp:982-1302`). The tagged-union native runtime example uses `printf`, so it is not used as the no-external-FFI proof. |
| `dyn` trait/interface dispatch | A no-FFI `dyn Area` program calls `Square.area` and `Circle.area`, returns 14, and passes LLVM verification (`tests/test-codegen.cpp:1290-1317`). The language specification documents this supported surface (`docs/Zith--.md:175-202`). | HIR represents erasure and vtable calls as `HirMakeDyn` and `HirDynCall` (`src/hir/hir-expr.hpp:352-379`); VM lowering has no handlers for either (`src/vm/hir-to-vm.cpp:982-1302`). The generic unsupported-expression message is the expected lowering path, but this native test has not been replayed with `--virtual-machine`. |
| Bare `opaque` | A no-FFI erase/check/extract program compiles and runs natively, returning 42 (`tests/test-codegen.cpp:387-413`). HIR tests verify the corresponding make/check/cast nodes (`tests/test-hir-lower-modern.cpp:978-1043`), and the language specification defines these operations (`docs/Zith--.md:624-628,654-665`). | `HirMakeOpaque`, `HirOpaqueCheck`, and `HirOpaqueCast` have no VM visitor handlers (`src/vm/hir-to-vm.cpp:982-1302`). They are rejected by the generic unsupported-expression path. |
| State machines | Native `state`/`jump`/`dock` execution returns 10 (`tests/test-codegen.cpp:1732-1747`); the specification lists these constructs as supported (`docs/Zith--.md:768-777`). | The HIR contract identifies `HirStateTailCall` as a terminating transfer (`src/hir/hir-expr.hpp:318-324`), but VM lowering does not handle it in `lowerOperand` or the terminator dispatch (`src/vm/hir-to-vm.cpp:982-1302,1593-1664`). Because the terminator switch has no final unsupported case, this may silently emit no transition rather than return status 5. The VM outcome is unverified and should be treated as a correctness-risk candidate, not a measured diagnostic. |
| Pipelines (`|>` / `do`) | A no-FFI pipeline program with `..` and a variadic slice compiles and runs natively, returning 60 (`tests/test-codegen.cpp:2358-2372`). HIR tests confirm the pipeline and current-value nodes are emitted (`tests/test-hir-lower-modern.cpp:2749-2770`). | `HirPipe` and `HirPipeCurrent` are absent from the VM visitor and reach its generic failure (`src/vm/hir-to-vm.cpp:982-1302`). This is a broader gap than variadic tails: the VM's completed variadic-slice support does not cover pipeline HIR itself. |

### Additional candidates requiring a paired VM run or scope check

- **Global constants:** ordinary Zith constants become `HirGlobalConstLoad`
  (`src/sema/hir-lower-expr-value.cpp:210-234`; reproducer in
  `tests/test-hir-lower-modern.cpp:360-386`). LLVM has a handler for this node
  (`src/codegen/codegen-emit-expr.cpp:29-38`), while the VM visitor does not
  (`src/vm/hir-to-vm.cpp:982-1302`). No native runtime codegen test for this
  exact node was located in this pass, so retain it as a high-confidence
  lowering candidate, not a paired finding.
- **`defer` cleanup:** the HIR lowerer emits `HirCleanup`, and LLVM executes its
  expressions (`src/sema/hir-lower-block.cpp:32-53`;
  `src/codegen/codegen-emit-stmt.cpp:77-84`). VM lowering has no expression
  handler for it (`src/vm/hir-to-vm.cpp:982-1302`). Existing native runtime
  cases use `putchar` (`tests/test-codegen.cpp:1865-1909`), so they do not
  establish an in-scope no-external-FFI repro.
- **Canonical type identity:** `@canonicalType(T)` lowers to
  `HirCanonicalType` (`tests/test-hir-lower-modern.cpp:1045-1072`) and LLVM
  emits the 128-bit identity (`src/codegen/codegen-emit-expr.cpp:460-470`).
  There is no VM handler. A native runtime parity case was not located.
- **Variadic `Formatable` rendering in console output:** the native parity case
  `console_println_format_int` succeeds and the VM path reports status 5, but
  the case's allow-list reason does not prove the failure cause. The VM
  fast-path check at `src/vm/hir-to-vm.cpp:571-600` inspects `call.args[0]`
  for a message slice, while the current API takes `allocator` before `msg`
  (`stdlib/std/io/console.zithm:275-284`). Thus the checked-in case does not
  establish that the placeholder guard is reached. Treat formatted rendering
  as a candidate gap requiring phase-specific evidence, not a confirmed cause.
- **Dynamic indexing outside arrays/slices:** the VM supports array and slice
  indexing, but rejects a non-literal index in its fallback path with
  `unsupported dynamic index in v2 lowering` (`src/vm/hir-to-vm.cpp:1178-1232`).
  This branch concerns other object kinds after the array/slice cases. The
  scope of native-accepted user programs that reach this branch needs a focused
  pointer-index repro before treating it as a general gap. Existing native
  tests confirm dynamic array indexing, which the VM handles, not this
  fallback (`tests/test-codegen.cpp:2215-2225`).

## Paired stdlib evidence and exclusions

The parity harness has seven case entries, and all seven carry a non-null
status-5 allow-list reason (`tests/test-stdlib-parity.cpp:47-156`). The verbose
baseline run reported status 5 for all seven. The parity document lists only
four under “Status-5 programs”; the other three console-literal cases are
misplaced in its “Passing programs” section. The harness requires the native
command to succeed before comparing VM behavior (`tests/test-stdlib-parity.cpp:296-327`).
This configured build has LLVM enabled, so the native command does select
native codegen (`build/CMakeCache.txt:419`; `src/cli/cmd/run.cpp:60-70`).

The formatted `println` case is among the seven status-5 cases, but its
allow-list reason does not prove that formatting is the cause. As noted above,
the current `println` API places the allocator before the message, while the
fast-path guard checks the first argument. The hash-map case may indicate an
additional HIR branch-shape limitation, but the stdlib path also uses
allocator/runtime externs; keep it conditional until the project decides
whether its validated VM runtime externs fall inside the agreed FFI boundary.
The allocator dynamic-dispatch case similarly combines `dyn` with
allocator/runtime externs. The opaque `InPlace` case is a useful no-user-C-FFI
confirmation of the opaque gap, though the parity table does not expose the
actual failure detail; it records only the static status-5 allow-list reason.

External global access is deliberately excluded. The VM emits a named
unsupported-global error for a referenced imported global, and the source is
C interop (`src/vm/hir-to-vm.cpp:1005-1010`; `tests/test-vm-v2.cpp:1285-1360`).
The FFI extern whitelist, C header import, and C ABI behavior are also not
counted as language lowering gaps.

## Shared causes and dependencies

- Most confirmed gaps converge on the incomplete HIR visitor. `hir::HirExpr`
  includes many more variants than `lowerOperand` handles
  (`src/hir/hir-expr.hpp:456-463`), and the visitor's default branch returns one
  generic unsupported-expression diagnostic (`src/vm/hir-to-vm.cpp:1296-1302`).
- Optional operations are a dependency cluster: checked indexing/slicing,
  `must`, and tagged-union/opaque narrowing can synthesize `Some`, `None`, or
  runtime-panic HIR. Supporting only the source-level optional type would not
  cover these generated paths.
- `dyn` and `opaque` share the general representation gap but need distinct
  semantics: `dyn` carries data plus a vtable and performs indirect dispatch,
  while bare `opaque` carries data plus a runtime type identity
  (`src/hir/hir-expr.hpp:352-419`).
- State transitions and runtime panic are terminators, not ordinary value
  expressions. Their missing terminator handling is a separate risk from the
  catch-all expression diagnostic.
- Struct and pack aggregate lowering, indirect function references, and
  variadic slice tails are documented as completed. They are not included as
  gaps (`docs/implementation-debt.md:162-165`; `docs/plans/vm-v2.md:39-52`).

## Native/spec discrepancies and uncertainty

No native-versus-`docs/Zith--.md` discrepancy was confirmed in this pass. The
spec explicitly includes `dyn`, enums/unions, optional conditions, opaque
operations, pipelines, and state/dock/jump in the active language surface
(`docs/Zith--.md:175-246,490-503,624-665,768-777`). Where the VM has no
corresponding HIR handler, the evidence points to a VM gap, not a reason to
change the language contract.

The paired harness proves native success and VM status 5 for its seven
allow-listed cases, but does not print the actual failure phase or observed
detail. For other rows, native tests plus emitted HIR plus the VM visitor
establish a strong source-level gap; this pass did not rerun every native test
under `--virtual-machine`. In particular, state-tail-call and runtime-panic
handling may fail by malformed or incomplete VM output rather than a clean
unsupported diagnostic. The suspected hash-map branch-shape cause and the
global-constant, cleanup, canonical-type, and variadic-formatting rows need
focused paired execution before being promoted to fully confirmed
behavior-level findings.

## Reproduction pointers

- Paired stdlib slice: `cmake --build build --target test-stdlib-parity`, then
  `ctest --test-dir build -R parity --output-on-failure`.
- Direct VM route for a checked-in native repro: run the same source with
  `zithc --virtual-machine run <source>`; for HIR-only evidence, use
  `--emit-hir`/the `HirLowered` session path before VM lowering.
- Useful checked-in repro sources are embedded in
  `tests/test-codegen.cpp:387-413` (opaque),
  `tests/test-codegen.cpp:562-579` (enum),
  `tests/test-codegen.cpp:1290-1317` (`dyn`),
  `tests/test-codegen.cpp:1732-1747` (state),
  `tests/test-codegen.cpp:2136-2150` (raw union),
  `tests/test-codegen.cpp:2358-2372` (pipeline), and
  `tests/test-codegen.cpp:2813-2823` (optional panic).
