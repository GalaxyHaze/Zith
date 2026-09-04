# Allocator, InPlace and Discard Contract

Zith-- needs a small ownership/cleanup contract that matches the current
subset: no full NRA proof, no automatic `drop`, and no compile-time proof that
a value was allocated by a particular allocator. This ADR separates a
convenient heap pair (`new`/`delete`) from a generic allocator pair
(`make`/`release`), keeps construction in an `InPlace` trait on the object,
defines two warning levels for discarded values, and records the first
stdlib-only allocation surface that is implemented today.

Status: proposed

## Contract

```zith
trait InPlace {
    fn inplace(var self, allocator: dyn Allocator, args: opaque): bool;
    fn clean(var self, allocator: dyn Allocator) {}
}

trait Allocator {
    fn alloc(self, size: u64, align: u64): ?raw opaque;
    fn free(self, mem: raw opaque, size: u64, align: u64);
    fn realloc(self, old: raw opaque, old_size: u64, old_align: u64,
               new_size: u64, new_align: u64): ?raw opaque;
}

fn new<T: InPlace>(args: opaque): ?*T;
fn delete<T: InPlace>(ptr: *T);

fn make<T: InPlace>(allocator: dyn Allocator, args: opaque): ?*T;
fn release<T: InPlace>(allocator: dyn Allocator, ptr: *T);
```

The shipped `stdlib/std/alloc.zith` currently implements the `Allocator` raw
storage surface plus `HeapAllocator` and the free-function bridge
`allocate`/`deallocate`/`reallocate` over `dyn Allocator`. A draft
`stdlib/std/new.zith` contains `InPlace`, `new<T>`/`delete<T>`, and
`make<T>`/`release<T>` as the target contract, but it is marked proposed and
is not part of the checked stdlib surface. The compiler cannot yet instantiate
generic helpers that mention `T` only in the return type, and opaque pack
fields cannot be extracted/destructured for in-place construction.

`make`/`release` are free generic functions rather than methods of the
`Allocator` trait. `Allocator` only owns allocation storage primitives and
does not need to know the layout of every `T`. `make<T>` reserves the layout
for `T`, calls `T.inplace`, and returns `?*T`; `release<T>` calls `T.clean`
and then asks the allocator to free the block. This also keeps custom
allocators implementable with a small, non-generic trait surface.

`new<T>` is sugar over a default heap allocator: it allocates the size and
alignment from the compiler-provided layout query for `T`, calls `T.inplace`,
and returns `?*T`. `delete<T>` calls `T.clean` and then frees through the
matching `HeapAllocator` methods. `make<T>` is the generic form and
`release<T>` is the generic teardown.

`alloc` returns an optional `raw opaque` because a failing allocation is a
normal control-flow result, not a panic. `free` carries the original
`size`/`align` so arena/pool allocators and future placeholder-based allocators
can validate or reclaim without adding a lookup table. `realloc` is required so
types such as `FormatBuffer` do not have to fall back to raw `malloc/free`
internally; a pass-through implementation may still return `null` when the
allocator cannot grow blocks in place.

`make<T>` uses the compiler-provided layout query for `T`, not a trait method
named `sizeOf`, because an allocator cannot know the layout of every generic
`T` through `Self`.

For the first version only `HeapAllocator` is required to implement
`realloc`. Other allocators may keep a const-failing `realloc` as long as
`make`/`release` still work. An allocator that cannot grow blocks in place
should document that and let `FormatBuffer`-style types allocate a fresh block
and copy.

The caller passes constructor arguments as a pack literal:

```zith
struct Buffer {
    bytes: ?*u8,
    len: u64,
}

implement Buffer as InPlace {
    fn inplace(var self, allocator: dyn Allocator, args: opaque): bool {
        let tuple = args as |cap: u64, fallback: u8|;
        if (tuple is null) {
            return false;
        }
        let data = allocate(allocator, cap, @alignOf(u8));
        if (data is null) {
            return false;
        }
        self.bytes = data;
        return true;
    }
}

fn main(): i32 {
    let buffer = new<Buffer>(|1024, 0 as u8|);
    if (buffer is null) {
        return 1;
    }
    delete<Buffer>(buffer);
    return 0;
}
```

The exact lower-level `Allocator` surface may start with only `alloc`/`free`
enough for `HeapAllocator`; pool/arena allocators can add private methods
later without changing the `InPlace` construction contract.

## Ownership and Warnings

An owner-managed type implements `InPlace`, or has a legacy `destroy` method
that delegates to `clean`. Non-owner temporaries are still subject to a lighter
warning when an expression result is ignored.

- `W10xx DiscardedValue`: a value expression is discarded without a binding,
  assignment, parameter, or explicit `_ = expr`. This is a general intent
  warning and does not imply resource cleanup.
- `W11xx DiscardedOwner`: an owner-managed value is discarded without a
  consuming `clean`/`delete`/`release` path, or an owner binding leaves scope
  without such a path. Examples include `new<Buffer>(...)` ignored as a
  statement and `InputLine` leaving a function without `destroy`.

The initial owner marker is the presence of `InPlace` or the legacy `destroy`
method on a struct. `?T`, `[]T`, pointers, and other non-trivial types are not
treated as owners by this ADR; they may only trigger `DiscardedValue`.

## Known Limitation: No Allocator Origin Proof

The current Zith-- compiler cannot prove that a pointer came from a specific
`new`/`make` call or allocator instance. Consequently:

- `delete<T>` cannot verify the object was heap-allocated or was created by
  the default heap allocator.
- `release<T>` cannot verify the object uses the allocator being passed.
- The owner warnings can track local `?*T` results and `InPlace`/`destroy`
  bindings in the same function, but they cannot prove cross-function pairing.

This is documented as an accepted limitation for the first version. The
contract intentionally keeps the pair `new`/`delete` and `make`/`release`
lexically visible so a future ownership pass can teach the compiler to reject
or warn about mismatched cleanup. Until that pass exists, the compiler should
not silently inject cleanup and should not claim to have proven that a
cleanup call is correct.

## Work Items

1. Register `InPlace` and `Allocator` as capabilities (or first-class traits)
   and add shape validation for `inplace`/`clean`/`alloc`/`free`.
2. Shipped: implement `HeapAllocator` and the `dyn Allocator` free-function
   bridge (`allocate`/`deallocate`/`reallocate`).
3. Teach generic inference to use return types so `new<T>`/`make<T>` can be
   instantiated from `?*T`; then enable `stdlib/std/new.zith` in the checked
   stdlib surface.
4. Implement opaque pack matching/destructuring for `make` argument dispatch
   and define the "no matching method" path as `null`.
5. Add `W10xx DiscardedValue` and `W11xx DiscardedOwner` in sema.
6. Keep `InputLine.destroy` working as a compatibility path and migrate it to
   `InPlace.clean` when the conventional `new`/`delete` surface is ready.
7. Add focused tests for: `new`/`delete`, custom `make`/`release`, opaque pack
   dispatch, `DiscardedValue`, `DiscardedOwner`, and `InputLine` without
   changing `cast<T>`.

## Consequences

- `new`/`delete` become the easy path for heap-allocated values; custom
  allocators use `make`/`release`.
- Cleanup is split: the object releases internal resources via `clean`, and
  the allocator releases the block via `release`/`delete`.
- The first version is intentionally conservative: warnings, not automatic
  cleanup, and no attempt to verify allocator origin.
- `cast<T>` keeps its current `view` semantics; `input().cast<i32>()` may warn
  as a discarded owner because `InputLine` is discarded, not because `cast`
  consumes or leaks the line.
