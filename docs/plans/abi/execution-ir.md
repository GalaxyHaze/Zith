# Execution IR And Interpreter Contract

Status: signed

Signed by `docs/adr/0018-execution-ir-interpreter-contract.md`.

Size: system, first slice is a single unit.

## Problem

Zith needs two portable execution paths and an IR that a future tiny backend
can consume. The repository currently documents an Interpreter HIR as the
portable path, but there is no executable interpreter under `src/` and no
explicit contract for the IR that a VM would run.

The HIR interpreter is the simple path and is selected explicitly by
`--interpreted`. The execution IR contract has two consumers in mind: the
later IR VM and a future tiny backend. The IR VM is the default runtime
execution path when LLVM or native codegen is not used. The idea keeps both
contracts in one drawing until the implementation split is justified.

## Non-Goals

- No LLVM codegen changes.
- No optimizer or SSA lowering in the first slice.
- No full Zith-- surface in the first slice.
- No WASM-specific runtime work beyond reusing the existing playground path.
- No new long-term CLI flag for the IR VM. The IR VM is selected by runtime
  availability, while `--interpreted` remains the explicit HIR path.
- No struct layout contract until the layout rows and target ABI are named.

## Decisions

These decisions are settled. Each one has a named reversal below because the
plan is now signed and a later change must make a new drawing.

1. **One execution IR contract, two consumers**.

   Chosen: a single execution IR contract serves the IR VM and the future tiny
   backend. The HIR interpreter consumes HIR and is a separate runtime
   contract. The execution IR splits only if the VM and tiny backend disagree
   on IR shape.

   Reversal: the tiny backend proves that the VM-target IR cannot be lowered
   without losing required semantics.

2. **Two interpreters**.

   Chosen: the project will have two runtime paths. The HIR interpreter is
   simpler and comes first. It serves explicit `--interpreted` execution,
   WASM, and builds without LLVM. The execution IR interpreter is the later,
   more optimized VM. It is the default runtime execution path when LLVM or
   native codegen is not used, and it also becomes the tiny backend input.

   Reversal: the HIR interpreter stays usable but the IR path becomes the only
   portable path.

3. **IR shape: register-based**.

   Chosen: the execution IR uses registers, not slots plus arena and not a
   stack machine. The concrete register model is a promise to compute after
   this drawing.

   Reversal: register lifetime analysis makes the first symmetric slice too
   expensive. Slots become the first runtime model instead.

4. **Call ABI v1: ordinary functions and a small extern subset**.

   Chosen: the first runnable slice for either runtime supports ordinary
   functions and a minimal set of common `extern fn` stubs. `state`, `dyn`,
   `opaque`, and variadic slices are separate promises later.

   Reversal: a hello-world test needs one of those surfaces to run. That
   surface moves into the first slice instead.

5. **Checks live by runtime shape**.

   Chosen: the HIR interpreter keeps checks inside the interpreter. The
   execution IR emits explicit trap instructions because it is register-based
   and must stay literal. The HIR path does not need trap instructions in the
   first slice.

   Reversal: the two paths diverge on a check semantic that one interpreter
   cannot reproduce through its adopted model.

6. **Minimal host runtime is in scope**.

   Chosen: the runtime FFI handler surface is included, but it is small: a
   minimal C subset, no complete libc. The first interpreter owns the handler
   table and reports a clear trap when a linkage name has no handler.

   Reversal: a common extern call proves that the minimal subset cannot stay
   compatible. The handler surface must be reduced instead.

7. **Repository layout: `src/ir/` and `src/interp/`**.

   Chosen: the execution IR lives in `src/ir/`, the interpreters live under
   `src/interp/`, and `--interpreted` selects only the HIR interpreter. The
   IR/VM is the default runtime execution path when LLVM or native codegen is
   not used. `src/vm/` is avoided as a separate runtime vocabulary.

   Reversal: the interpreter and the compiler need the IR types in a shared
   frontend location. The source layout moves without changing the contract.

8. **First conforming seams: hello-world tests**.

   Chosen: the first ABI tests are standalone tests under `tests/`. They lower
   a small HIR module and verify a hello-world program through the HIR
   interpreter (`ABI-EXEC-09`) and through the execution IR VM (`ABI-EXEC-11`).
   WASM playground reuse remains a later integration.

   Reversal: the standalone host paths cannot prove WASM behavior. A WASM test
   joins a later slice instead.

9. **IR stores metadata only**.

   Chosen: the IR stores metadata and contracts. The implementation behind it
   is independent, so a later backend can keep its own internal execution
   engine while accepting the same IR. Layout rows for the IR instruction
   header are not promised in this slice.

   Reversal: an IR consumer needs binary-instruction layout before it can read
   the IR. Layout rows become part of the contract instead.

## Next Step

Run the next lifecycle slice after the signed promises in
`docs/adr/0018-execution-ir-interpreter-contract.md`. The HIR interpreter
and the execution IR VM hello-world seams are implemented. The next slice
expands the IR/VM support surface or wires the WASM playground runtime.

## Technical Facts

The CLI routes `--interpreted` through the HIR interpreter and, on builds
without LLVM or native codegen, routes plain `run`/`execute` through the
execution IR VM. The WASM playground exports `zith_run_source`, but it only
runs the compiler stages up to HIR and does not execute the program.

The compiler already produces an in-memory `hir::HirModule` before codegen.
HIR contains explicit slots (`HirSlotAlloca`, `HirSlotStore`, `HirSlotLoad`,
`HirSlotAddr`), calls (`HirCall`), branches, jumps, and terminators. This is
the ingestion seam for a HIR interpreter.

ZIRL persists HIR bodies in a cache artifact and already supports hydration
back into `HirModule`. A cache-backed interpreter can consume the same HIR
that codegen consumes today.

The WASM playground has one stable host import: `zith.host_write(stream, ptr,
len)`. The minimal host runtime can target that seam directly and does not
need a full libc in the first slice.

## Promises (Signed)

These promises are signed by
`docs/adr/0018-execution-ir-interpreter-contract.md`.

### ABI-EXEC-01

```text
id: ABI-EXEC-01
contract: Execution IR contract
rule: The repository defines one execution IR contract in src/ir/ that the
IR VM and the future tiny backend share, and no second IR format enters the
first slice. The HIR interpreter consumes HIR and does not define execution
IR layout.
seam: tests/test-abi-execution.cpp and source review of src/ir/
layout: none
fields: none
reverse: A tiny backend consumer proves it cannot share the VM IR without
losing required semantics.
```

### ABI-EXEC-02

```text
id: ABI-EXEC-02
contract: HIR interpreter
rule: `zithc --interpreted` on a host build lowers a program to `HirModule`
and executes it with the HIR interpreter without invoking LLVM codegen or a
linked native binary. The flag does not select the execution IR path.
seam: tests/test-abi-execution.cpp
layout: none
fields: none
reverse: The CLI becomes non-LLVM capable but still links a native executable
before executing the interpreter path.
```

### ABI-EXEC-03

```text
id: ABI-EXEC-03
contract: HIR interpreter, ordinary functions
rule: The HIR interpreter executes functions with bodies, parameters, local
slots, calls, branches, jumps, and returns for the Zith-- surface exercised by
the hello-world test.
seam: tests/test-abi-execution.cpp
layout: none
fields: none
reverse: A required Zith-- feature forces the interpreter to rely on LLVM
semantics that are not present in HIR.
```

### ABI-EXEC-04

```text
id: ABI-EXEC-04
contract: HIR interpreter, minimal extern set
rule: The HIR interpreter resolves a minimal set of extern linkage names
through the Runtime FFI handler table, and returns a trap when a linkage name
has no handler.
seam: tests/test-abi-execution.cpp
layout: none
fields: none
reverse: A hello-world program needs a libc function outside the minimal set
before the interpreter can show end-to-end output.
```

### ABI-EXEC-05

```text
id: ABI-EXEC-05
contract: Execution IR shape
rule: The execution IR contract is register-based. Instructions address named
registers and do not encode stack-machine or slot-only semantics in v1.
seam: source review of src/ir/ plus a later IR interpreter test
layout: none
fields: none
reverse: Register lifetime analysis makes the first IR interpreter slice too
expensive, and the project chooses slots before registers.
```

### ABI-EXEC-06

```text
id: ABI-EXEC-06
contract: Checks and traps
rule: The HIR interpreter keeps runtime checks inside the interpreter, and the
execution IR expresses runtime checks as explicit trap instructions in its
contract.
seam: tests/test-abi-execution.cpp and source review of src/ir/
layout: none
fields: none
reverse: The two runtime paths adopt different observable failure behavior for
the same source program.
```

### ABI-EXEC-07

```text
id: ABI-EXEC-07
contract: Minimal host runtime
rule: The first interpreter supports a minimal C-compatible runtime surface
for WASM and no-LLVM host builds, and does not require a complete libc.
seam: tests/test-abi-execution.cpp and src/wasm/playground.cpp
layout: none
fields: none
reverse: A common extern call proves that the minimal surface cannot stay
stable, and the runtime grows beyond the declared subset.
```

### ABI-EXEC-08

```text
id: ABI-EXEC-08
contract: CLI and repository layout
rule: The execution IR lives under src/ir/, the interpreters live under
src/interp/, `--interpreted` selects the HIR interpreter, and the IR/VM is
the default execution path when LLVM or native codegen is not used. src/vm/
is not introduced for the runtime.
seam: source layout review and tests/test-abi-execution.cpp
layout: none
fields: none
reverse: The CLI keeps `--interpreted` but routes it to the IR/VM, or a
no-LLVM default still links a native binary instead of selecting the IR/VM
path.
```

### ABI-EXEC-09

```text
id: ABI-EXEC-09
contract: Hello-world conforming seam
rule: A standalone test under tests/ lowers a small Zith-- source file, runs
the HIR interpreter, and verifies the program output and exit status. WASM
playground reuse and the IR/VM seam are later integrations, not the first
seam.
seam: tests/test-abi-execution.cpp
layout: none
fields: none
reverse: The standalone host path cannot reproduce WASM behavior, and a WASM
test becomes the first conforming seam.
```

### ABI-EXEC-10

```text
id: ABI-EXEC-10
contract: IR metadata contract
rule: The IR stores metadata and contract information for consumers, and the
implementation behind each interpreter or tiny backend stays independent.
No binary instruction layout is promised in this slice.
seam: source review of src/ir/ and the later IR interpreter
layout: none
fields: none
reverse: An IR consumer needs binary-instruction layout before it can read
the IR, and layout rows become part of the signed contract.
```

### ABI-EXEC-11

```text
id: ABI-EXEC-11
contract: IR/VM hello-world seam
rule: A standalone test under tests/ lowers a small Zith-- source file to the
execution IR and runs it with the IR VM, verifying program output and exit
status. The HIR interpreter is not used on this path.
seam: tests/test-abi-execution.cpp
layout: none
fields: none
reverse: The IR VM needs HIR-level checks or a second interpreter to reproduce
the hello-world behavior, and the explicit-trap contract is not sufficient.
```
