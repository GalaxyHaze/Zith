# Zith-- Beta Module List

Date: 2026-11-24
Compiler: `Zith` HEAD `29444d4e`, build tree `build/`.

This is the Step 4 deliverable "module purpose, import path, maturity,
ownership/resource cleanup expectations, and one compiling example for every
module in the beta list". It classifies every module shipped under `stdlib/`
as `std` stable, `soon` experimental, or `c` raw C binding, and it keeps the
advertised beta surface to the modules the verified examples and first-use
tasks need. `docs/impl-status.md` remains the status authority and
`docs/20-standard-library.md` remains the library reference.

## Classification Rule

- `std` stable: a module the verified examples or the `zithc create`
  first-use path already imports, with isolated type checking and runtime
  coverage for each documented behavior.
- `c` raw: a thin, simple portability layer over C declarations. It is not a
  safe wrapper.
- `soon` experimental: a module that exists and type-checks but is not part of
  the advertised beta surface. `soon/` is not on the default include path.

The `soon`-first promotion gate lives in
`docs/plans/stdlib/soon-promotion.md`. A module moves from `soon/` to `std/`
only after it clears that gate. Do not promote a module to enlarge the beta
feature list.

## Stable `std` Surface

| Module | Import path | Purpose | Cleanup | Evidence |
|---|---|---|---|---|
| Console I/O | `std/io/console` | `print`/`println`/`eprint`, `input()`, `InputLine` with `cast<T>`, `Formatable`, `ParseInput` | `InputLine` owns its buffer; call `destroy` after use | `tests/test-codegen.cpp::test_from_console_lowers_println_body`, `::test_console_alias_resolves_member_without_global_import`; `examples/styles-zith.zithm` runs to exit 0 |
| Formatting | `std/io/format` | `FormatBuffer`, `format`, `appendRawBuffer`, `appendNumberBuffer`, `Formatable` implementations | `emptyFormatBuffer` allocates; call `destroyBuffer` after use | `tests/test-stdlib-io.cpp::test_io_format_returns_owned_result`, `::test_io_format_into_existing_buffer` |
| Named-block scope | `std/scope` | `Scope`/`Guard` traits and the `@scoped` marker macro for managed enter/leave | `@scoped` registers `leave` via `defer`, so cleanup runs on every exit | `tests/lang/tests/named_blocks.zithm` (corpus) |
| Test helpers | `std/test` | `assert*`/`check*` macros and the failure reporters behind `@assert`/`@check` | Reporter buffers allocate on failure and are freed inside the reporter | `tests/test-zith-test-suite.cpp`, `tests/lang/tests/stdlib_assertions.zithm` (corpus) |
| Memory facade | `std/memory` | Re-exports the `soon/memory` DAG: `Allocator`, `HeapAllocator`, `InPlace`, `allocate`/`deallocate`/`reallocate`, `new`/`delete`, `make`/`release` | `new`/`delete` own construction and cleanup; raw `allocate` storage is caller-owned | `tests/test-interface-satisfaction.cpp`, `tests/test-generic-hashmap.cpp`, `test-codegen` memory pairs |

Compiling examples:

```zith
// std/io/console + std/memory (the `zithc create` scaffold shape)
from std/io/console
from std/memory

fn main(): i32 {
    let heap = HeapAllocator {}
    println(heap, "hello from console")
    0
}
```

```zith
// std/io/format
from std/io/format
from std/memory

fn main(): i32 {
    let heap = HeapAllocator {}
    var buffer = emptyFormatBuffer(heap)
    _ = appendRawBuffer(lend buffer, "n=", 2);
    _ = appendNumberBuffer(lend buffer, 42);
    destroyBuffer(lend buffer)
    0
}
```

```zith
// std/scope
from std/scope

pub struct Sink {}
pub struct SinkGuard {}

implement Sink as Scope {
    fn enter(var self): dyn Guard {
        return SinkGuard {};
    }
}

implement SinkGuard as Guard {
    fn leave(var self) {}
}

fn main(): i32 {
    var sink = Sink {}
    render: @scoped(sink) {
        return 0;
    }
}
```

```zith
// std/test
from std/test

fn main(): i32 {
    @assertEq(2 + 2, 4);
    0
}
```

```zith
// std/memory
from std/memory

fn main(): i32 {
    let heap = HeapAllocator {}
    var p = allocate(heap, 16u64, 8u64)
    if (p is null) { return 1; }
    deallocate(heap, p, 16u64, 8u64)
    0
}
```

Every snippet above passes `zithc --include stdlib check`. The console,
memory, and dyn-array snippets also run to exit 0 on the native x86-64 target.

## Raw `c` Surface

| Module | Import path | Purpose | Cleanup | Evidence |
|---|---|---|---|---|
| Console C functions | `c/io` | `putchar`, `getchar`, `puts`, `fputs` and friends | None; these are raw calls | `tests/test-stdlib-io.cpp` (`c/fs` surface check imports it transitively) |
| Allocation C functions | `c/stdlib` | `malloc`, `calloc`, `realloc`, `free`, `aligned_alloc` | Caller owns the returned storage and must `free` it | `tests/test-stdlib-io.cpp::test_c_fs_portable_surface_checks` |
| String/byte C functions | `c/string` | `memcpy`, `memset`, `strlen`, `strncmp`, `strcpy`, `strcat` | None; these are raw calls | `tests/test-stdlib-modules.cpp` (isolated type check) |
| File C functions | `c/fs` | `FILE`, `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `remove`, `rename`, `SEEK_*` | A `?*FILE` is null on failure and must be closed on every path | `tests/test-stdlib-io.cpp::test_c_fs_portable_surface_checks` |
| Math C functions | `c/math` | `sin`, `cos`, `tan`, `fabs` and friends | None; these are raw calls | `tests/test-stdlib-modules.cpp` (isolated type check) |
| POSIX (Linux) | `c/posix` | `open`, `close`, `read`, `write`, `lseek`, `stat`, `mmap`, `munmap`, `O_*`/`PROT_*`/`MAP_*` | Descriptors and mappings are caller-owned; close and unmap on every path | `tests/test-stdlib-io.cpp::test_c_posix_variant_resolves_on_linux` |

`c/posix` ships as the `.<os>` platform variant `c/posix.linux.zithm`, so the
import resolves only on Linux. These are raw bindings, not a safe wrapper. A
`c/math` call links only when the host toolchain links `libm`; a program that
calls `sin`/`fabs` fails at link time on a target where `-lm` is not applied,
so `c/math` carries a native-link limitation rather than a runtime promise.

## Experimental `soon` Surface

None of these modules are part of the advertised beta API. They exist,
type-check under `test-stdlib-modules`, and several have native runtime
coverage, but they do not clear the `soon`-promotion gate.

| Module | Import path | Purpose | Notes |
|---|---|---|---|
| Result | `soon/result` | Positional `Result<T, E>` union with `Ok`/`Err`/`isOk`/`isErr`/`unwrap`/`unwrapErr` | Native-tested (`test_imported_result_runtime`); VM v2 and per-promise contract tests pending |
| Result fallback | `soon/result/fallback` | Checked `unwrapOr`/`unwrapErrOr` over `soon/result` | Native-tested; VM v2 pending |
| Panic | `soon/panic` | Fatal `panic(message)` through `R10004` | Native-tested (`test_imported_panic_runtime`) |
| Memory DAG | `soon/memory/in-place`, `soon/memory/allocators/allocator`, `soon/memory/allocators/heap`, `soon/memory/new` | `InPlace`, `Allocator`, `HeapAllocator`, `new`/`delete` | Native-tested; the `std/memory` and `soon/memory` facades re-export them |
| DynArray | `soon/collections/dyn_array` | Generic `DynArray<T>` with explicit allocator | `examples/dyn-array.zithm` runs to exit 0 |
| Hash map (concrete) | `soon/collections/hash_map_u64` | Concrete `u64 -> u64` open-addressed map | `examples/hash-map-u64.zithm` runs to exit 0 |
| Hash map (generic) | `soon/collections/hash_map` | Checked generic `HashMap<K, V>` with `Hashable` | `examples/hash-map.zithm` runs to exit 0 |
| String | `soon/string` | Owning byte string with a tagged-union iterator | Native and VM v2 parity case `soon_string_iterator_end_member`; lacks per-promise contract tests |
| Counter | `soon/counter` | `Counter` struct with a `next(): ?i32` method | Native-tested (`test_imported_counter_runtime`) |
| Math | `soon/math` | `pi` plus `sin`/`cos`/`tan`/`fabs` wrappers over `c/math` | Native link needs `libm`; type-checks only |
| Math scalar | `soon/math/scalar` | `minI32`, `maxI32`, `clampI32`, `degreesToRadians`, `radiansToDegrees` | Native-tested but link needs `libm` (`fabs`) |
| Math vec | `soon/math/vec` | Vector helpers | Type-checks only |
| Math transform | `soon/math/transform` | Transform helpers | Type-checks only |
| Slice algorithms | `soon/algorithms/slice` | `findIndexI32`, `countI32` over pointer-plus-length slices | Native-tested (`test_soon_slice_algorithms_runtime`) |
| Alloc/new helpers | `soon/alloc`, `soon/new` | Convenience re-exports over the memory DAG | Type-checks only |
| File system stub | `soon/fs/fs` | Placeholder owning-file wrapper | Stub; the safe wrapper is deferred behind `c/fs` |
| JSON | `soon/json` | `parse`/`stringify`/`isValid` | Stub; explicitly excluded from the beta API |

## Evidence Base

- `cmake --build build -j4`: exit 0.
- `ctest --test-dir build -R 'test-stdlib-modules|test-stdlib-io|test-stdlib-parity|test-examples'`:
  4 of 4 passed on 2026-11-24.
- `test-stdlib-modules` type-checks every `stdlib/**.zithm` in isolation.
- `zithc --include stdlib check` passes on every snippet above.
