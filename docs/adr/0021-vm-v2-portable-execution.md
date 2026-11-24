# VM v2 Portable Execution Is The Target Backend For Browser And No-LLVM Run

Zith will keep two portable runtime paths. The HIR interpreter is explicit
with `--interpreted` and the signed execution IR v1 remains the current
no-LLVM/native fallback, but VM v2 becomes the long-term portable target for
browser and no-LLVM `run`/`execute`. VM v2 uses a typed linear register IR,
guest-offset linear memory, indexed `fn`/`extern fn` references, and a small
validated FFI subset instead of SSA or full libc.

Status: accepted (host slice)

## Contract Scope

VM v2 is the portable execution contract that produces typed assembly-like IR
from HIR. Allocators are stdlib/runtime constructs on top of VM memory
primitives. The first acceptance slice is two Hello World shapes:
`from std/io/console` plus `println`, and manual `extern fn` with a small
`stdio.h`-style FFI surface. State machines, `dyn`, `opaque`, closures,
variadic slices, and full C header parsing remain later slices.

## Promises

### VMV2-01: IR v2 shape is linear and typed

The IR v2 contract uses typed registers and explicit opcodes for arithmetic,
memory, control and calls. It is not an SSA/optimizer IR in the first slice.
Seam: `src/vm/typed-ir.hpp` and `tests/test-vm-v2.cpp`.

### VMV2-02: `fn` and `extern fn` use indexed references

Function values and extern pointers are typed references into the module
function/extern table, not raw C function pointers.
Seam: source review of `src/vm/` and a later indirect-call test.

### VMV2-03: allocators stay above the VM primitives

The IR/VM exposes allocation primitives (`AllocBytes`, `MallocBytes`,
`malloc`) but not allocator APIs. `soon/memory`/`HeapAllocator` run in terms of
these primitives.
Seam: `tests/test-vm-v2.cpp` for primitives; stdlib tests for higher layers.

### VMV2-04: FFI is a validated small subset

The VM FFI handler set is limited and explicit. `malloc`, `free`, `putchar`,
and the `snprintf` formats used by stdlib are the first surface; missing or
unsupported externs trap.
Seam: `tests/test-vm-v2.cpp` and the runtime FFI handler table.

### VMV2-05: first end-to-end acceptance

A host test runs both a `from std/io/console` program and a manual
`extern fn`/stdio-style program through VM v2 and compares output and exit
code with the native path.
Seam: a dedicated browser/no-LLVM VM v2 tests/ executable.

### VMV2-06: VM v2 remains separate from execution IR v1 but is promoted

The signed execution IR v1/`src/ir` path is not changed by this slice. The
host VM v2 sources are promoted into `zithcLib` and selected by the CLI only
when LLVM/WASM is unavailable. `--interpreted` keeps the HIR interpreter.
Seam: CMake source glob, `src/cli/cmd/run.cpp`, and `tests/test-abi-execution.cpp`
stability.
