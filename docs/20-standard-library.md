## 20. Standard Library

> **Implementation status:** `stdlib/c/io.zith` and `stdlib/std/io/console.zith` are the only
> shipped modules. `puts` and `println` work. All other standard library content is **spec-only**.
> See [impl-status.md](impl-status.md).

### 20.1 Three-Part Structure

| Namespace | Stability | Use when |
|---|---|---|
| `std` | Stable, backward-compatible | You need a guaranteed API |
| `soon` | Experimental, may change | You're prototyping and don't mind breakage |
| `c` | Direct C FFI bindings | You need to call C APIs |

```zith
import std;
import soon;   // use with caution — API may shift
import c;       // raw C bindings
```

### 20.2 Core Modules

#### `std/io/console`
```zith
fn println(msg: []char): void;
fn print(msg: []char): void;
fn eprint(msg: []char): void;
```

```zith
@println("hello");
```

#### `std/collections/DynArray`

```zith
struct DynArray<T> {
    fn push(self: lend, val: T);
    fn pop(self): ?T;
    fn len(self): u64;
    fn get(self, index: u64): ?T;
}
```

#### `std/fs`
```zith
struct File { ... }

fn open(path: string): File!;
fn read(self: view File): []u8!;
fn write(self: lend File, data: []u8): void!;
```

### 20.3 Common Traits

| Trait | What it enables |
|---|---|
| `Copy` | Bitwise copy — primitives and components get this by default |
| `Clone` | `fn clone(self): Self!` |
| `Lent` | Can appear as a `lend` parameter |
| `Share` | Safe to share across threads |

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
