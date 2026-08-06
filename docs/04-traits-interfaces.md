## 4. Traits, Interfaces & Capabilities

> **Implementation status:** `trait`, `interface`, and `implement T as Trait {}` declarations are
> **working** (parsed, resolved, and lowered). Trait dispatch and vtable-based `dyn` dispatch are
> **spec-only** — `dyn Trait` is a parse error in type position.
> See [impl-status.md](impl-status.md).

### 4.1 Traits vs. Interfaces

| | Trait | Interface |
|---|---|---|
| **Typing** | Nominal — must be explicitly implemented. | Structural (duck-typed) — automatically satisfied when fields match. |
| **Extensible** | Yes — via `extends`, or as a precondition using `requires`. | No — interfaces cannot extend each other, though a trait may `requires` one. |
| **Has implementation?** | Yes — default method bodies are allowed. | No — declaration only. |
| **Field access** | Only through a trait that `requires` the interface. | Yes — directly, since interfaces are structural. |

### 4.2 Traits

A `requires Cond` clause goes **before** `trait` or `interface`. It forces any implementing type to also satisfy that condition. Traits may provide default method bodies. Use `self` / `other` as the conventional instance parameters, and `Self` (capitalized) for the concrete implementing type.

```zith
trait Printable {
    fn print(self);
    fn println(self) { self.print(); io.writeln(""); }
}

requires Printable
trait JsonSerializable {
    fn print(self);
    fn toJson(self): string;
}

// Disambiguate overlapping method names using the trait as a namespace
Printable.print(self);
JsonSerializable.print(self);
```

### 4.3 Interfaces

Interfaces are structural — if it quacks, it's a duck. Any type that has the required fields satisfies the interface automatically, without an explicit `implement` declaration. You can also add `requires` to interfaces.

```zith
// will only accept structs and reject components
requires @isStruct
interface iPositioned { [x, y, z]: f32 }

requires iPositioned
trait Movable {
    fn translate(self, dx: f32, dy: f32, dz: f32) {
        self.x += dx; self.y += dy; self.z += dz;
    }
}

// Any struct with x, y, z: f32 satisfies iPositioned automatically
struct Enemy { [x, y, z]: f32, health: i32 }
```

### 4.4 Capabilities — Built-in Reference

Capabilities are special traits that feed the compiler more information, unlocking special rules and optimizations.

| Capability | What it does |
|---|---|
| `Copy` | Implicit bitwise copy. Components and primitives are `Copy` by default. |
| `Functor` | `operator()` — makes a type callable like a function. |
| `Arithmetic` | Operators `+`, `-`, `*`, `/`, `%`, and so on. |
| `Error` | `operator throw`, required for `throw MyError;`. |
| `Null` | A negative capability — its traits activate only once NRA has proven a value IS `null`. Outside the proven-null branch, calling the method is a **compile error**. |
| `Fail` | A negative capability — its traits activate only once NRA has proven a value IS an error. Cannot coexist with `Null` on the **same level**, but `?T!` can have `Null` on the outer level and `Fail` on the inner. |
| `Allocator` | To provide custom allocators |
| `Generator` | Allows creating runtime-defined resumable or streaming protocols without introducing a dedicated core function kind. |
| `Share` | Required for `global: share` and crossing thread boundaries |
| `Lent` | Enables `global: unique`, a runtime-checked exclusive borrow. `global` bindings cannot be moved — `Lent` manages thread-safe distribution. Also allows `lend` parameters. |
| `Trust` | A trait extending `Trust` may contain `raw fn` methods callable from safe contexts. |
| `Unique` | Marks a singleton type. It cannot be instantiated — the type name itself acts as the instance. All fields must implement `Share` (thread-safe). A `unique Local` variant is a singleton thread-local. |

#### `Null` & `Fail` — Negative Capabilities

Dispatch is based on NRA state. Inside a proven-null branch, the `Null` trait unlocks. Outside it, calling `Null` methods is a **compile error**:

```zith
implement Config as Null {
    fn onMissing(self) { log("Config was null -- using defaults"); }
}
implement Config! as Fail {
    fn onError(self) { log("Config load failed"); }
}

let cfg: ?Config = loadConfig();
if (cfg is null) {
    cfg.onMissing();   // OK — Null trait unlocked here
}
// cfg.onMissing();   -- COMPILE ERROR: outside null branch

// Multi-level: ?T! — Null on outer, Fail on inner
// cfg.onError() is only valid inside a proven-error branch
```

`Null` and `Fail` are per-level. A `?T!` value can activate `Null` (outer `?`) independently from `Fail` (inner `!`).

#### `Trust` — Safe Sections with Raw Code

```zith
trait Place extends Trust {
    raw fn sample(): i32 {}
}

fn safeCaller(a: Place) {
    let v = a.sample();
}
```

#### `Unique` — Singleton Types

```zith
struct AppConfig: Unique { host: string, port: u16 }

AppConfig.host = "localhost";
AppConfig.port = 8080;
// let cfg = AppConfig { ... };  -- COMPILE ERROR
```

#### `Share` — Mutable State Across Thread Boundaries

```zith
// Share is mutable, multiple names, multiple threads, all can write
global counter: share Atomic<i32> = 0;

// Without the Share capability, cross-thread access is a compile error
struct LocalOnly { data: i32 }
// global bad: share LocalOnly = ...;  -- COMPILE ERROR: lacks Share
```

### 4.5 Operator Overloading

Operator overloading is capability-based and strict — you implement only the operators you need:

```zith
implement Vec3 as Arithmetic {
    fn +(self, other: Self): Self { ... }
    fn -(self, other: Self): Self { ... }
}

implement Pipeline as Functor {
    fn (self, input: []u8): []u8 { ... }
}
let out = pipe(raw_bytes);
```

### 4.6 Extension (`extends`)

`extends` copies the base type's fields and traits into the new struct. An optional `:` after the base lists further traits to implement:

```zith
struct Dog extends Animal {}
struct T extends Base: Transform, Collision {}

// Traits may also extend capabilities or other traits
trait SafeBuffer extends Trust {
    raw fn readByte(self, offset: u64): u8 {}
}
```

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
