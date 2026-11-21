# Stdlib Allocator and Ownership Contract

The target stdlib-only allocator surface is `stdlib/std/memory*`. The strict
module graph is `in-place` -> `allocator` -> `heap` and `new` depends on
`in-place` plus `heap`. The original `stdlib/std/alloc.zithm` and
`stdlib/std/new.zithm` remain as legacy compatibility modules. The compiler
supports trait-based dynamic dispatch through `dyn Allocator` free-function
helpers, but concrete trait method calls still invalidate the receiver after a
`self`/`var self` call. The surface keeps raw storage primitives, a heap
allocator, and free functions that go through `dyn`.

## Compiler Constraints That Shaped the API

`var self` on `Allocator` methods made a concrete `HeapAllocator` unusable for
more than one call: `let mem = h.alloc(...); h.free(...)` reported `E4001
cannot use 'h' after it was moved by a previous call`. `lend`/`view` receivers
did not match the trait requirements and produced `E2022` signature errors.
`self` without an explicit qualifier works as a read-only receiver and is the
shape used by `Formatable` elsewhere in the stdlib.

Calling a method on `dyn Allocator` inside a free function works repeatedly:

```zith
fn allocate(self: dyn Allocator, size: u64, align: u64): ?raw opaque {
    return self.alloc(size, align);
}
```

Calling `h.alloc(...)` twice on a concrete `HeapAllocator` in the same scope
currently fails with `E4001` even with read-only trait receivers. Users should
go through `std.memory.allocate` and `std.memory.deallocate` for now.

An owning type can now retain its allocator in a `dyn Allocator` field: a
struct literal coerces a concrete field value to `dyn` at the literal site, so
`Resource { allocator: heap }` passes `check` and `run`. This previously
failed under `zithc run` with `E5001 Invalid InsertValueInst`; see
`test-codegen`'s `test_dyn_trait_struct_literal_field_runtime`.

Zith-- bitwise operators use the `&.` spelling, and negative null checks use
`not (value is null)`. Bare `&` and `is not null` fail during parsing.

## Module Resolution Quirk

The legacy `std/alloc` failed with `E2001 unknown struct type` while `std/alloc3`
with identical content imported correctly. The empty directory
`stdlib/std/alloc/` was the cause: the resolver treated the import as a
directory module. The resolver now prefers regular-file candidates over a
same-named directory, so the facade remains usable when leaf modules share its
path prefix.

`export` paths in a facade may share a namespace prefix (`export std/a` plus
`export std/b`). The frontend injects each target's public symbols and keeps an
independent `ModuleAlias` for every exported namespace prefix, so `from
std/memory` exposes the leaf contracts and `import std/memory` can resolve each
fanout path such as `std.memory.in-place.InPlace` without a spurious `E2002
duplicate binding`.

The `std/memory` facade shares its logical path with the `stdlib/std/memory/`
directory. Regular-file precedence selects `memory.zithm` before directory
aggregation, allowing the facade to export nested paths such as
`std.memory.allocators.heap.HeapAllocator`. Cold and cached coverage lives in
`tests/test-interface-satisfaction.cpp`.

## Design Intent

The target proposal in `docs/adr/0010-allocator-inplace-drop.md` separates:

- `Allocator`: storage primitives `alloc`/`free`/`realloc` with `?raw opaque`.
- `InPlace`: object construction/cleanup hooks for allocator-based values. The
  trait does not name an allocator, so the module graph stays DAG-shaped. An
  imported `implement Box as InPlace` with qualified trait calls is covered by
  `tests/test-generic-hashmap.cpp`.
- `new<T>`/`delete<T>` and `make<T>`/`release<T>`: heap and generic allocator
  convenience pairs. `stdlib/std/memory/new.zithm` now carries real bodies:
  `new<T>` queries `@sizeOf`/`@alignOf`, allocates through the `HeapAllocator`
  bridge, calls `(*p).inplace(args)`, and returns `?*T`; `delete<T>` calls
  `(*p).clean()` and frees the block. `stdlib/std/new.zithm` remains as
  compatibility. The allocator-owned pair ships as free functions
  `make`/`release` over `dyn Allocator` in `allocators/allocator` (the
  `Allocator.make`/`release` defaults are const-failing until a trait default
  body can dispatch to another trait method on `self`).
- `W10xx DiscardedValue` and `W11xx DiscardedOwner`: future compiler warnings;
  no codegen changes are shipped in the first stdlib-only step.

## `std/memory/new.zithm` Compiler Gaps

`stdlib/std/memory/new.zithm` is the single source for the `InPlace`/`new`/
`delete` contract. The module, the `InPlace` trait, and the `new<T>`/`delete<T>`
bodies all pass `zithc check`, link, and run (`test-codegen`
`test_imported_memory_new_delete_runtime`). The remaining gap is the
allocator-owned `make<T>`/`release<T>` default methods, not the heap pair.

Resolved blockers:

- Contextual (expected-result) inference now binds a result-only parameter such
  as `T` in `fn new<T>(args: opaque): ?*T` from a binding annotation or
  `return`, so `let p: ?*Box = new(0 as opaque);` resolves without explicit
  `<...>`. See `memory/comptime-contextual-inference.md`.
- The in-place constructor is reached with `(*p).inplace(args)` on the raw
  pointer (the `p.InPlace.inplace(...)` and `p->InPlace.inplace(...)` spellings
  are not accepted for a pointer receiver).

Still open:

- `make<T>`/`release<T>` are trait **default methods** on `Allocator`. A trait
  default body cannot yet dispatch to another trait method on `self`
  (`self.alloc(...)` reports `E3001 field access on non-struct type`), so the
  generic pair stays a const-failing default until that lands.
- Resolved: opaque pack values can now be destructured and field-accessed.
  `let tuple = args as |cap: u64|;` followed by `if (tuple is null) { return; }`
  and `tuple.cap` (or `self.cap = tuple.cap`) compile and run. The early-return
  guard narrows the optional aggregate, and `x.cap` extracts the payload field.
  Aggregate literals erased into an `opaque` parameter also keep their concrete
  type now (a one-field struct no longer corrupts the erased payload).
  A bare `null` member in a pack literal is resolved too: the surrounding
  annotation supplies the member type, and `null as ?T` is a valid explicit
  spelling. The canonical `new<Buffer>(|null as ?*u8, 0u64|)` call runs, and an
  untyped `|null, 0u64|` reports `E3001` instead of corrupting the aggregate.
  See `memory/sema-nonnull-narrowing.md`.
- The order-dependent global snapshot scans that made imported trait
  conformance unstable in populated workdirs are gone. Sema now resolves
  imported traits, type names, and owner methods through the current module's
  import bindings and namespace aliases; `tests/test-interface-satisfaction.cpp`
  covers populated-workdir qualified calls with `from std/memory` and several
  unrelated modules. Trait-default collection and dyn method lookup still scan
  all loaded modules where the default/requirement was defined, so a future
  compatibility change to method-scope resolution can narrow those scans
  further.
- `@alignOf` now accepts any complete type (primitives and generic `T`
  included) and types as `i32`, so heap `new<T>`/`make<T>` can query alignment
  for primitive-layout `T`; `@sizeOf` still types as `u64`. `@alignOf(T)`
  therefore needs an explicit `as u64` when combined with `@sizeOf(T)` in the
  same `u64` expression.

`destroy` remains object-owned for the legacy I/O types. The allocator owns
only raw storage. `HeapAllocator` uses C `malloc`/`free`/`realloc` and ignores
alignment in the heap path.
