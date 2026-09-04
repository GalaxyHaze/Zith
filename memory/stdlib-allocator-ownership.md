# Stdlib Allocator and Ownership Contract

The stdlib-only allocator surface lives in `stdlib/std/alloc.zith`. The
current compiler supports trait-based dynamic dispatch through `dyn Allocator`
free-function helpers, but concrete trait method calls still invalidate the
receiver after a `self`/`var self` call. The module deliberately keeps the
first shipped allocation surface small and request-scoped: raw storage
primitives, a heap allocator, and free functions that go through `dyn`.

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
go through `std.alloc.allocate` and `std.alloc.deallocate` for now.

## Module Resolution Quirk

`std/alloc` failed with `E2001 unknown struct type` while `std/alloc3` with
identical content imported correctly. The empty directory
`stdlib/std/alloc/` was the cause: the resolver treated the import as a
directory module. Removing the empty directory and clearing the stale
`.zith-cache` directories restored `from std/alloc` and
`import std/alloc as a`.

## Design Intent

The full proposal in `docs/adr/0010-allocator-inplace-drop.md` separates:

- `Allocator`: storage primitives `alloc`/`free`/`realloc` with `?raw opaque`.
- `InPlace`: object construction/cleanup hooks for allocator-based values.
- `new<T>`/`delete<T>` and `make<T>`/`release<T>`: heap and generic allocator
  convenience pairs. `stdlib/std/new.zith` carries the target trait and helper
  signatures as a proposed draft, but it is not registered in `test-examples`
  because the compiler cannot check it yet.
- `W10xx DiscardedValue` and `W11xx DiscardedOwner`: future compiler warnings;
  no codegen changes are shipped in the first stdlib-only step.

## `std/new.zith` Compiler Gaps

The API draft in `stdlib/std/new.zith` exists as the single source for the
`InPlace`/`new`/`delete`/`make`/`release` contract, but `zithc check` cannot
accept it yet. Confirmed blockers:

- Generic inference only unifies function parameters, not results. A helper
  such as `fn new<T>(args: opaque): ?*T` fails with `E3011 cannot infer
  generic argument; provide explicit type arguments inside an importing
  module`, and even an explicit `new<Box>(...)` call cannot force resolution
  once `opaque` packs and `dyn Allocator` are involved.
- Opaque pack values cannot be destructured/field-accessed in the current
  subset, so `args as |cap: u64|` compiles for matching but assigning
  `self.cap = raw tuple` fails with `E3001 expected 'i32', has type 'pack'`.
- `implement` of an imported trait is unstable when the module also imports
  `std/alloc`; keeping `new.zith` outside the checked/tested surface avoids
  locking users into an importing module that currently contaminates symbol
  resolution with unrelated `alloc.zith` diagnostics.
- `@alignOf` only accepts structs in the current subset, so heap
  `new<T>`/`make<T>` cannot query alignment for primitive-layout `T` without
  compiler work.

`destroy` remains object-owned for the legacy I/O types. The allocator owns
only raw storage. `HeapAllocator` uses C `malloc`/`free`/`realloc` and ignores
alignment in the heap path.
