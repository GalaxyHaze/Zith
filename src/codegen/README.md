# Codegen v2

`src/codegen` is the experimental backend facade for lowering `toolkit::sir` into
flat, arena-backed bytecode. It remains separate from `toolkit::session` and has no
dependency on the session generators or CLI.

## Purpose

- `codegen.hpp` exposes `Op`, `FunctionCode`, `ByteCode`, `CodegenState`, `Backend`,
  and `codegen(module)`.
- `vm/` is a self-contained `zct_codegen_vm` target. The VM has no strong type table:
  it copies and interprets raw bytes, resolves slot offsets from the layout encoded by
  `FunctionCode`, and decides widths from the arithmetic/return opcode.

## Architecture

The flattening pass is the only codegen layer that walks SIR. It validates structure,
types, and layout, and produces `paramBytes`/`localBytes` memory slots plus opcodes with
explicit widths (`I32=4`, `I64/F64=8`). The VM validates only argument byte arity against
`FunctionCode` before execution.

`ByteCode` owns a `DynArray<FunctionCode>`; each `FunctionCode` owns an arena-backed
`DynArray<Instruction>` and the byte layout for parameters and locals. The VM interprets one
function from raw argument bytes and returns `StdReturn { uint8_t raw[8] }`.

## Public API

- `Op::ConstI32/ConstI64/ConstF64`, `Op::AddI32/AddI64/AddF64`,
  `Op::SubI32/SubI64/SubF64`, `Op::MulI32/MulI64/MulF64`, `Load`, `Store`, and
  `RetI32/RetI64/RetF64/RetVoid`.
- `VM::run` accepts a span of raw bytes and returns `StdReturn { uint8_t raw[8] }`.
- `CodegenState::flatten(module)` validates and lowers SIR to `ByteCode`.
- `CodegenState::runBackend(module, Backend::VM)` runs the selected backend and returns
  `Result<ByteCode>`.

The v2 bytecode does not implement calls, branches, nested scopes, or unsupported
primitive widths.

## Usage Flow

1. Build a `toolkit::sir::Module` with the `SirBuilder` API.
2. Validate the module with `toolkit::sir::verify`.
3. Call `CodegenState{arena}.runBackend(module, Backend::VM)`.
4. Copy the returned `ByteCode` into a mutable local and pass `functions[0]` to `VM::run`
   with a byte span matching the function parameter layout.
5. Decode the raw `StdReturn` bytes according to the return opcode width.

## Minimal Example

```cpp
Arena arena;
SirBuilder builder(arena);
auto &module = builder.createModule("demo");
auto &add = module.declareFn("add", Primitives::i32,
                             ArgsDecl{Primitives::i32, Primitives::i32});
add.ret(add.add(add.param(0), add.param(1)));

auto bytecode = CodegenState{arena}.runBackend(module, Backend::VM);
VM vm;
const auto args = i32Args(3, 4); // 8 raw bytes, little-endian i32 pair
auto &fn = bytecode.value().functions[0];
const auto result = vm.run(fn, std::span<const std::uint8_t>{args});
```

The complete runnable form is `tests/codegen/codegen-demo.cpp`.

## Build

Codegen is disabled by default. Enable it and pick the backend before configuring:

```bash
cmake -S . -B build -DZCT_CODEGEN=ON -DZCT_CODEGEN_BACKEND=VM
cmake --build build -j --target codegen-test
ctest --test-dir build -R codegen --output-on-failure
```

`ZCT_CODEGEN_BACKEND` reserves `TINY`, `TINY_JIT`, `VM`, `LLVM`, and `LLVM_JIT`.
Only `VM` is accepted in this phase.

## Tests And Demo

`tests/codegen/codegen-test.cpp` covers the backend and VM surface. `codegen-demo` builds an
`add(i32, i32)` SIR function, lowers it, runs the VM with arguments `3` and `4`, and prints the
expected result `7`.

```bash
cmake --build build --target codegen-demo codegen-test -j
ctest --test-dir build -R '^codegen' --output-on-failure
```

## Agent Boundary

`src/codegen` is experimental handwritten backend code. Keep the flatten pass and VM focused on
arena-backed bytecode; do not add session or frontend dependencies. `src/symbols/`,
`src/common/import/`, generators, and `tools/rules_kit/` remain protected.
