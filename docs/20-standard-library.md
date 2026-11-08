## 20. Standard Library

> **Status:** see [impl-status.md](impl-status.md) for the current status of each module. The
> shipped modules are `stdlib/c/io.zith`, `stdlib/c/stdlib.zith`, `stdlib/c/string.zith`,
> `stdlib/c/fs.zith`, `stdlib/c/posix.linux.zith`,
> `stdlib/std/io/console.zith`, the `std/memory` DAG, `stdlib/std/collections/hash_map_u64.zith`
> (a concrete `u64 -> u64` map), `stdlib/std/collections/hash_map.zith` (a checked generic
> `HashMap<K, V>`), and `stdlib/std/result.zith` (the canonical positional
> `Result<T, E>` union with `Ok`/`Err`/`isOk`/`isErr`/`unwrap`/`unwrapErr` helpers). `std/alloc`
> and `std/new` remain as legacy compatibility modules.

`std`/`soon` remain documentation-only in this iteration, except for the shipped modules listed
above. The common C bindings used by those modules live under `c/` rather than importing host
headers directly.
The documented convention uses resource types with `init`/`destroy`, read-only methods with
`view`, and mutating methods with `lend`. `defer` runs `destroy(self: lend Self)` on resource
cleanup. `drop` remains outside the current Zith-- contract.

### 20.1 Three-Part Structure

| Namespace | Stability | Use when |
|---|---|---|
| `std` | Stable, backward-compatible | You need a guaranteed API |
| `soon` | Experimental, may change | You're prototyping and don't mind breakage |
| `c` | Supported C/runtime surface | You need common low-level or system APIs |

```zith
import std;
import soon;   // use with caution — API may shift
import c;       // supported C/runtime surface
```

The `c` namespace is not a request to parse a host header. It is the stable
low-level surface maintained by the compiler/runtime. Its declarations may map
to libc on a native LLVM target, to VM intrinsics on the no-LLVM backend, or to
host/runtime imports on WASM. The source API stays the same while the selected
backend chooses the concrete implementation.

Use `c/...` for common C-shaped facilities that Zith supports across targets:

```zith
from c/io
from c/stdlib

fn main() {
    puts("hello");
    let memory = malloc(64);
    free(memory);
}
```

Use `import "file.h"` for a target-specific declaration set, a platform API,
or an external C library that is not part of the supported `c` surface. Header
imports remain an interop escape hatch, not the mechanism used to bootstrap the
standard library.

### 20.2 Core Modules

#### `std/io/console`
```zith
fn println(msg: []char): void;
fn print(msg: []char): void;
fn eprint(msg: []char): void;
```

The console module also provides `input()` as the owning line-input
constructor. `InputLine` owns its buffer and exposes `text`, `len`, `good`,
`cast<T>`, and `destroy`. Callers must destroy the line after use.

`ParseInput` is the parsing contract implemented by the primitive numeric and
boolean types shipped with the standard library:

```zith
pub trait ParseInput {
    fn parse(self: view InputLine): ?Self;
}
```

`InputLine.cast<T: ParseInput>` returns `?T` without destroying the line.
Passing `view line` preserves the owner for later use:

```zith
let n = line.cast<i32>();
```

`i32`, `bool`, `f32`, `f64`, and `u32` implement `ParseInput` in the current
stdlib surface. `*char` parsing remains out of scope.

```zith
@println("hello");
```

#### `std/alloc`

Raw storage primitives and a default heap allocator. The trait methods use
`self` read-only receivers because the current compiler invalidates a concrete
receiver after a trait method call in the same scope. The `dyn Allocator`
free functions are the supported repeatable call path.

```zith
pub trait Allocator {
    fn alloc(self, size: u64, align: u64): ?raw opaque;
    fn free(self, mem: raw opaque, size: u64, align: u64);
    fn realloc(self, old: raw opaque, old_size: u64, old_align: u64,
               new_size: u64, new_align: u64): ?raw opaque;
}

pub struct HeapAllocator {}

pub fn allocate(self: dyn Allocator, size: u64, align: u64): ?raw opaque;
pub fn deallocate(self: dyn Allocator, mem: raw opaque, size: u64, align: u64);
pub fn reallocate(self: dyn Allocator, old: raw opaque, old_size: u64,
                  old_align: u64, new_size: u64, new_align: u64): ?raw opaque;
```

`allocate` returns `null` when the underlying `malloc`/`realloc` call fails.
The caller owns the storage and must pass the same `size`/`align` to
`deallocate`. The larger `InPlace`/`new`/`delete`/`make`/`release` contract is
recorded in [ADR 0010](adr/0010-allocator-inplace-drop.md). `stdlib/std/memory/new.zith`
ships real `new<T>`/`delete<T>` bodies, and `stdlib/std/new.zith` remains as
legacy compatibility. `new<T>` binds its result-only `T` from the expected
result type, so `let p: ?*Box = new(0 as opaque);` needs no explicit `<...>`;
it allocates through the `HeapAllocator` bridge, calls `(*p).inplace(args)`, and
`delete<T>` calls `(*p).clean()` and frees the block. The `InPlace` trait is
checked and covered by a conforming type in `tests/test-generic-hashmap.cpp`,
and the heap pair runs end-to-end in `test-codegen`
(`test_imported_memory_new_delete_runtime`).

#### `std/memory` (target DAG)

```zith
pub trait InPlace {
    fn inplace(var self, args: opaque): bool;
    fn clean(var self) {}
}

pub fn new<T: InPlace>(args: opaque): ?*T;
pub fn delete<T: InPlace>(ptr: *T);

pub fn make<T: InPlace>(self: dyn Allocator, args: opaque): ?*T;
pub fn release<T: InPlace>(self: dyn Allocator, ptr: *T);
```

The allocator-owned `make<T>`/`release<T>` pair ships as free functions over
`dyn Allocator` (`make(heap, args)` / `release(heap, ptr)`) rather than as
trait default methods, because a trait default body cannot yet dispatch to
another trait method on `self`. `make<T>` reserves the layout for `T`, calls
`(*p).inplace(args)`, and returns `?*T`; `release<T>` calls `(*p).clean()` and
frees the block through the same allocator. `Allocator.make`/`release` remain
const-failing defaults that custom allocators may override. The module is
intentionally not wired into `test-examples`; `examples/inplace-simple.zith`
continues to demonstrate the local `InPlace` shape.

The layer DAG is kept strict: `in-place` defines `InPlace` only,
`allocators/allocator` depends on `in-place` and owns `Allocator`,
`allocators/heap` implements `HeapAllocator`, and `new` depends on `in-place`
plus `allocators/heap` without naming `make`/`release`. `std/memory`
re-exports the same-level contracts, including `Allocator`, `HeapAllocator`,
the raw free-function bridge, and `new`/`delete`, so callers can use one stable
import root (`from std/memory`).

`export` paths that share a namespace prefix (for example the `std/...` roots
inside the facade) are deduplicated into one qualified namespace; the public
symbols of every export are injected independently. This keeps a facade usable
even when it re-exports more than one module under the same top-level segment.

#### `std/collections/DynArray`

```zith
struct DynArray<T> {
    fn push(self: lend, val: T);
    fn pop(self): ?T;
    fn len(self): u64;
    fn get(self, index: u64): ?T;
}
```

The generic `DynArray<T>` is consumable from another module through the same
imported-instantiation path as `HashMap<K, V>` (ADR 0020): `da.create<i32>(heap)`
constructs an array, `da.DynArray.reserve<i32>`/`da.push<i32>` grow it, and
`da.get<i32>`/`da.remove<i32>`/`da.pop<i32>` return `?T`. `examples/dyn-array.zith`
passes `zithc check` and runs to exit 0, and `test-codegen` exercises an imported
`DynArray<i32>` end-to-end.

#### `std/collections/hash_map_u64` (concrete)

The shipped concrete map stores `u64` keys and `u64` values. It uses an
open-addressed linked chain and owns its backing storage with `calloc`/`free`.
Callers must call `destroy` after use.

```zith
import std/collections/hash_map_u64 as hm;

var map = hm.HashMap { count: 0u64, capacity: 0u64, table: null, head: 0u64 };
if not(hm.HashMap.reserve(lend map, 64u64)) { /* allocation failed */ }
hm.HashMap.put(lend map, 1u64, 42u64);
let value = hm.HashMap.get(view map, 1u64); // ?u64
hm.HashMap.destroy(lend map);
```

#### `std/collections/hash_map` (checked generic)

`HashMap<K, V>`, `Entry<K, V>`, and `Hashable` form the generic checked map
module. Concrete generic instances are reified with their type arguments
stored as structural metadata on `StructType`, so nested `Entry<K, V>` slots
keep the correct `K`/`V` mapping and `current.key.hash()` resolves through the
`K: Hashable` bound.

The module is consumable from another module. `hm.HashMap<i64, i64>{}` reifies
from the declaring module's template, field defaults (`count = 0`,
`table = null`, ...) lower in the declaring module, and
`hm.HashMap.init(lend map)` inherits the receiver's type arguments. Nested
generic calls inside the body are concretized transitively, so
`init<i64,i64>` calls `reserve<i64,i64>`. `examples/hash-map.zith` passes
`zithc check` and runs to exit 0, and both `tests/test-generic-hashmap.cpp` and
`test-codegen` exercise an imported `HashMap<i64,i64>`. See
[ADR 0020](adr/0020-imported-generic-instantiation.md) for the design. For a
concrete shipped runtime example, the `u64 -> u64` module above remains
available.

#### `std/result`

The canonical error-handling foundation is a single positional generic union
plus generic helpers:

```zith
pub union Result<T, E> { T, E }

pub fn Ok<T, E>(value: T): Result<T, E>;
pub fn Err<T, E>(value: E): Result<T, E>;
pub fn isOk<T, E>(r: Result<T, E>): bool;
pub fn isErr<T, E>(r: Result<T, E>): bool;
pub fn unwrap<T, E>(r: Result<T, E>): T;
pub fn unwrapErr<T, E>(r: Result<T, E>): E;
```

The union must be `pub` for cross-module consumers; a non-public union fails
the importing module with `E2001`. Result-only parameters bind from the
expected result type, so `let r: Result<u32, u32> = Ok(7);` and
`return Ok(value);` need no explicit `<...>`. `unwrap`/`unwrapErr` perform no
tag check, so the caller must establish `isOk`/`isErr` first. The module is
type-checked by `test-stdlib-modules` and exercised end-to-end by `test-codegen`
(`test_imported_result_runtime`).

#### `std/panic`

`panic(message)` reports a fatal message and does not recover. It writes
`message` and a trailing newline, then terminates through the runtime panic
code `R10004` (`@assert(false)` outside a test). It is the standard-library
counterpart to the `@assert`/`@check` intrinsics and the building block for
unwrapping the wrong `Result` case. There is no catch and no cleanup, so it is
only for unrecoverable states.

The message is written one byte at a time through `putchar`, so the same path
works on native and VM v2. The native runtime panic helper flushes the C
streams before trapping, so the message survives even when the program's
output is a pipe. The module is type-checked by `test-stdlib-modules` and
exercised end-to-end by `test-codegen` (`test_imported_panic_runtime`).

#### `c/fs` and `c/posix`

Raw file access ships under `c/` now, not `std/fs`. `c/fs` is the portable C
stdio surface (`FILE`, `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`,
`fflush`, `feof`, `ferror`, `remove`, `rename`, and the `SEEK_*` constants).
`c/posix` is the Linux POSIX file and memory-mapping surface (`open`, `close`,
`read`, `write`, `lseek`, `stat`, `fstat`, `access`, `unlink`, `mmap`,
`munmap`, and the `O_*`, `PROT_*`, `MAP_*`, and access-mode constants); it
ships as the `.<os>` platform variant `c/posix.linux.zith`, so the import
resolves only on Linux.

```zith
from c/fs
from c/posix
```

These are raw bindings, not a safe wrapper. A `?*FILE` from `fopen` is null on
failure and must be closed on every path with `defer fclose(stream);`.
The safe `std/fs` wrapper (an owning `File` with `init`/`destroy` and a named
error enum) remains deferred behind the allocator-ownership, `DynArray`/
`Result`/`panic`, and mutable `lend`/`dyn` receiver work. See
[ADR-0046](adr/0046-c-file-surface-and-deferred-safe-std-fs.md).

### 20.3 Common Traits

| Trait | What it enables |
|---|---|
| `Copy` | Bitwise copy — primitives and components get this by default |
| `Clone` | `fn clone(self): Self!` |
| `Lent` | Can appear as a `lend` parameter |
| `Share` | Safe to share across threads |

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
