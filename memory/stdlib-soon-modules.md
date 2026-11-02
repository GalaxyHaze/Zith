# `soon/` Modules And The Stdlib Module Guard

`stdlib/soon/` holds experimental modules that may move to `std/` after the
promotion gate in `docs/plans/stdlib/soon-promotion.md`. The current surface
includes root modules for alloc, counter, math, memory, new, panic, result, and
string, plus the fs stub, collections, memory/allocators, result/fallback,
math/scalar, math/vec, math/transform, and algorithms/slice.

## Module Guard Test

`tests/test-stdlib-modules.cpp` walks `stdlib/**.zith` and type-checks every
module in isolation through the real frontend/sema pipeline (registered as
`test-stdlib-modules`). It exists because no prior test compiled each module on
its own, so whole modules could rot: `soon/string.zith` was documented as
"implemented" while failing `zithc check` with four syntax/semantic errors, and
`soon/json.zith` used an invalid declaration form. Run it before shipping any
`stdlib/` edit; a single failing module fails the test with the module name.

## `soon/string.zith` Fixes

The module is heap-backed and stores `data: ?*char`, `length: u64`, and
`allocator: dyn Allocator`. The bugs the guard now prevents:

- `if (...) { return expr }` without a trailing `;`. Zith-- requires
  `return expr;`; a bare `return` before `}` is the only value-less form. Four
  sites (`startsWith`, `concat` x2, `destroy`) were affected.
- `stringIterator { self.data, ... }` wrote a `?*char` into a `*char` field.
  Fix: `raw self.data` at the literal site.
- `deallocate(..., data as raw opaque, ...)` after an early `if (data is null)
  { return; }` still fails E3003. Narrowing is only tracked through a
  positive `not (x is null)` guard, not an early-return negative guard, so the
  fix wraps the free in `if (not (data is null)) { ... }`.
- C calls whose result is dropped need `_ = call();` (with the semicolon);
  `strcpy`/`strcat` return `?*char`.

`soon/json.zith` used `mod(2) fn test()`; `mod(N)` depth is only valid on
struct/interface fields, not on top-level functions. The stub uses `mod fn
test()`.

## Co-Located Test Direction

Some `soon/` files declare `mod fn test()`, but module visibility is not test
discovery. The `zithc test` runner discovers `#[test]` functions in files under
the configured `test_dir`; the module-local functions are not run automatically.
Runtime coverage for the moved helpers currently lives in C++ fixtures such as
`tests/test-codegen.cpp`. To make tests genuinely co-located and executable,
use the `#[test]` runner contract rather than treating `mod fn test()` as a
test by convention.

## Cross-Module Generic Call Binding

`GenericCallBinding::module` identifies the caller module. In HIR call
resolution, match a concrete generic function against the target module on
`InstantiationInstance::module`, not the binding's caller module. Otherwise,
equal declaration IDs from different modules can resolve a call to the wrong
generic function. `test_imported_result_runtime` in `tests/test-codegen.cpp`
covers imported `Result` constructors and `soon/result/fallback` helpers.

## Stable Facade Boundary

Do not remove `std/` facades as part of the `soon/` promotion work. `std/` is
not only a compatibility layer: shipped modules such as `std/io/console`,
`std/io/format`, `std/result`, and `std/test` remain active. The console module
imports `std/io/format`. `tests/test-interface-satisfaction.cpp` imports the
`std/memory` facade and writes qualified types under `soon.memory...`, because
a re-export keeps the target's own path prefix. The facade still injects the
public symbols (`from std/memory` exposes `HeapAllocator` and `InPlace`), but a
qualified name resolves through the re-exported `soon/...` module path, so the
`std.memory...` spelling does not name a type. Cold and warm cache loads are
both covered. Broader removal requires a separate stable-API migration.

## Facts And Gotchas

- The extraction path for a union is `raw value as Member`; direct
  construction is `Result<A, B>{ value }`.
- `soon/result/fallback` builds on `std/result` and adds checked
  `unwrapOr`/`unwrapErrOr`; it does not duplicate `Result<T, E>`.
- `soon/math/scalar` is allocator-free and provides `minI32`, `maxI32`,
  `clampI32`, `degreesToRadians`, and `radiansToDegrees`.
- `soon/algorithms/slice` provides allocator-free `findIndexI32` and
  `countI32` over pointer-plus-length slices. Zith currently rejects equality
  on generic type parameters and also fails to type-check a function-valued
  equality callback in this generic call path, so the first algorithms stay
  concretely typed until those compiler paths are supported.
- `soon/` is not on the default include path. Tests and users need
  `--include stdlib` (CLI) or `opts.includeDirs.push(ZITH_STDLIB_DIR)`.
- The promotion gate wants one contract test per promise id, native + VM v2
  runs, and no open decisions; `soon/string.zith` still lacks signed promises.

## Cross-Module Tagged-Union `is Member` (fixed)

The `stringIterator.next()` step is a tagged union (`stringCharStep`) whose
`End` member is tested from the importing module as `step is s.End`. That check
silently lowered to an always-false test, so the iterator loop never broke and
`soon/string` reads overran the buffer. Root cause: HIR lowering resolved the
`is` target with the raw `TypeTable::lowerTypeExpr`, which only knows bare
names via `named_registry_` and returns `kInvalidTypeId` for a qualified name
like `s.End`. Both `HirLowerModern::lowerTypeExprConcrete`
(`src/sema/hir-lower-types.cpp`) and the `is`-narrowing path in
`src/sema/hir-lower-block.cpp` now route qualified `Name` type expressions
through the module's `PerModuleSema::lowerTypeExpr`, which resolves import
bindings. Regression coverage: `test_imported_tagged_union_is_member_runtime`
in `tests/test-codegen.cpp` and the `soon_string_iterator_end_member` parity
case in `tests/test-stdlib-parity.cpp` (native + VM v2).
