# Generic union instantiated with `void`/`never`

The declaration path rejects a literal `void` union member ("union member
cannot be 'void'", `src/sema/sema-decl.cpp`). A **generic** union could still
substitute `void` through a type argument, for example `union Pair<T> { T, i32 }`
used as `Pair<void>`, or stdlib `Result<void, E>`. That `void` member reached
codegen, where `lower(TypePack)`/`getTypeAllocSize(void)` makes LLVM loop at
100% CPU on `--emit-ir` (`check`/`--emit-hir` pass, only LLVM codegen hangs).

## Fix

`PerModuleSema::instantiateTemplateFromArgs` (`src/sema/sema-type.cpp`, the
`DeclKind::Union` case) now checks each resolved member and rejects a member
whose kind is `Void` or `Never` with `E3001`, pushing `error_type` and
continuing. This mirrors the declaration path, so the bad union is never
interned and never reaches codegen.

The diagnostic is reported at the **use site** (`span`, the caller's type
expression) with a note on the generic member that resolved to `void`, rather
than pointing at the template declaration line. For an imported template the
note points into the declaring module (`stdlib/std/result.zithm`), which is
expected.

Regression: `test_generic_union_void_member_rejected` in
`tests/test-enum-union-generics.cpp` asserts `Pair<void>` is rejected with
`E3001`.

## Not implemented

This only turns the hang into a clean rejection. `Result<void, E>` full support
(a unit/void representation for a union member) is a separate, larger change;
`memory/stdlib-io-result.md` records that it is out of scope for the first
stdlib I/O version. The stdlib `Result<T, E>` helpers (`Ok`/`Err`/`isOk`/...)
live in `stdlib/std/result.zithm`.
