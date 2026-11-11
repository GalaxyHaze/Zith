# Foreign C Enum Values Use Their Clang-Reported Type

Imported C enums now lower to their libclang-reported integer type. The HIR
lowerer registers foreign enums reachable from imported function signatures,
globals, typedefs, pointers, and record fields so a named sema placeholder used
by a local has the same integer representation as the imported function ABI.

## Failure That Led to the Fix

Found on 2026-11-08 while validating the C ABI. Before the fix, an imported
enum parameter produced `E5001` because LLVM received a `void` parameter. An
enum return used as a value hung codegen.

The binder originally discarded `clang_getEnumDeclIntegerType()`. Capturing
that type fixed direct function signatures, but not a local variable: sema
represented the imported enum as a named placeholder, and `lowerType()` had no
foreign metadata for that placeholder. It therefore returned `kErrorType`,
which reached codegen as `void`.

## Current Lowering

- `src/cinterop/c-header.cpp` stores the underlying integer type reported by
  libclang. A missing type is not guessed.
- `HirLowerModern::registerForeignTypes()` indexes imported records and enums
  reachable through function signatures, globals, typedefs, pointers, and
  record fields before lowering function bodies.
- `lowerType()` completes imported enum placeholders from that registry.
  Pointers to incomplete enums remain opaque. A by-value enum without known
  underlying metadata is rejected.

## Regression Coverage

`tests/test-cinterop.cpp` checks that an ordinary C enum reports an integer
underlying type of 32 bits on the configured target.
`tests/test-codegen.cpp` compiles and runs a C function returning an enum,
stores the result in a Zith local, and passes it to a second C function.

The codegen fixture stores `color.h` at its root and `color.c` under `c/`, so
the C source includes it as `../color.h`. Test executables are emitted directly
under `build/`, for example `build/test-codegen` and `build/test-cinterop`.
