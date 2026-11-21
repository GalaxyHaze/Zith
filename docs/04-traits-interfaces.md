## 4. Traits, Contracts & Capabilities

> **Status:** see [impl-status.md](impl-status.md) for the current status of `trait`, `interface`,
> and `implement` declarations. Full Zith calls the structural declaration `contract`, uses
> `extend T with Trait` for nominal conformance, and reserves `extends` for trait composition.
> References to `interface` and `implement` below describe the existing Zith-- surface. Full-Zith
> examples use the updated spellings. See
> [ADR-0040](adr/0040-full-zith-surface-names-and-keyword-limit.md).

The owner of an `extend` block may be a primitive, `?T`, or `[]T` in addition to a named struct
or enum:

```zith
trait Describe {
    fn describe(self): i32;
}

extend i32 with Describe {
    fn describe(self): i32 { return 1; }
}
```

Conformance for these owners is nominal, just like conformance for named types. These traits are
usable in generic bounds (`T: Describe`) and expose default methods on the receiver. Pointer and
fixed-array owners remain unsupported.

> See [impl-status.md](impl-status.md).

### 4.1 Traits vs. Contracts

| | Trait | Contract |
|---|---|---|
| **Typing** | Nominal. A type explicitly conforms to a trait. | Structural. A type satisfies a contract automatically when all its conditions hold. |
| **Purpose** | Names behavior and capabilities a type opts into. | States a static contract that a type or value must satisfy. |
| **Composition** | `extends` composes traits. | Bounds combine contracts with `+`. |
| **Methods** | Requirements and optional default bodies. | Exact-signature requirements without default bodies. |
| **Fields** | Available through a contract required by the trait. | Guarantees field existence and type; qualifiers constrain mutability, while the memory access mode determines whether writes are permitted. |

### 4.2 Traits

Traits are nominal contracts. A type must explicitly implement a trait. Trait
methods may have default bodies. Trait clauses follow the name on separate
lines, in the order `extends` then `requires`. `extends` composes traits.
`requires Contract` requires the trait's `Self` type to satisfy that contract.
The requirement propagates to generic bounds, so `T: Movable` also gives the
body the contract guarantees required by `Movable`. Use `self` / `other` for
value parameters and `Self` for the implementing type.

```zith
contract Positioned {
    [x, y]: i32
}

trait Movable
    requires Positioned
{
    fn moveBy(self, dx: i32, dy: i32) {
        self.x += dx;
        self.y += dy;
    }
}

fn move<T: Movable>(value: T, dx: i32, dy: i32) {
    value.moveBy(dx, dy);
}
```

### 4.3 Contracts

A contract is a static structural requirement, not a behavior trait or a dynamic-dispatch type. A
type satisfies a contract automatically when its type-level and value-level conditions hold. No
explicit trait-conformance declaration is needed. Contract conditions combine conjunctively.
Repeated identical conditions count once, while contradictory or incompatible conditions make
satisfaction invalid.

```zith
contract SafeNormalize requires @struct {
    [x, y]: i32,
    @ensure(self.y is not 0)
}
```

Header `requires` constraints such as `@struct` are checked when the compiler
evaluates whether a type satisfies the contract.
A field requirement guarantees that the field exists with the declared type.
Code under the contract bound may read that field. Field qualifiers can
constrain mutability, while the memory access mode still controls whether a
write is permitted:

```zith
contract Normalize {
    var [x, y]: i32,
    @ensure(self.y is not 0)
}
```

Unqualified `[x, y]` follows the mutability available through the passed value.
With a `lend` value, it requires mutable fields and permits writes through that
access. With `view`, access remains read-only regardless of the field
declaration.
`let [x, y]` promises that the fields are immutable while the contract is
active. `var [x, y]` requires mutable fields; writing still requires a mutable
access mode such as `lend`. In particular, `view` remains read-only, even for
fields declared `var`; it never permits interior mutation. `var` does not add
implicit synchronization or relax cross-thread safety requirements.

A method requirement names an exact signature. Calls through a contract bound
use static dispatch, and contracts cannot provide method bodies.

`self` conditions are checked at the call boundary and remain invariants while
the structural contract is active. Writes and calls must preserve them. The
compiler must prove each `@ensure`; if it cannot, compilation fails rather than
inserting a runtime check. `@assume`, `@ensure`, and `@maybe` keep their
existing meanings. In particular, `@assume` is diagnosed when it contradicts
facts already known to the compiler.

Capability-use annotations and purity are separate contract dimensions.
`@using` requires a reachable use of each named capability, while `@only`
restricts which capabilities may be used. `@pure`, `@honest`, and `@impure`
describe function result behavior and side effects. All are static obligations
with no runtime checks. See
[ADR-0039](adr/0039-full-zith-capability-and-purity-contracts.md) for their
semantics and [ADR-0044](adr/0044-full-zith-annotation-order-and-purity-rename.md)
for the canonical annotation order and the `@honest` rename.

Combine contracts and behavior explicitly in a generic bound:

```zith
fn normalize<T: SafeNormalize + Arithmetic>(value: T): T {
    // The contract and trait bounds are both required.
    value
}
```

The full-Zith model does not define `dyn contract`. Zith-- currently supports method dispatch
through `dyn Interface`; that implementation behavior is documented separately and is not part
of the full-Zith contract model.

### 4.4 Capabilities — Built-in Reference

Capabilities are special traits that feed the compiler more information, unlocking special rules and optimizations.

| Capability | What it does |
|---|---|
| `Copy` | Implicit bitwise copy. Components and primitives are `Copy` by default. |
| `Functor` | `operator()` — makes a type callable like a function. |
| `Arithmetic` | Operators `+`, `-`, `*`, `/`, `%`, and so on. |
| `Error` | Marks a type as an error value. It does not define whether a value is valid or invalid. |
| `Failable` | Defines a type's valid and invalid states. The compiler invokes its state-checking contract for `try`. |
| `Invalid` | A `Failable` type may conform to it to expose methods available only after flow analysis proves that its value is invalid. |
| `Allocator` | To provide custom allocators |
| `Generator` | Allows creating runtime-defined resumable or streaming protocols without introducing a dedicated core function kind. |
| `Share` | Required for `global: share` and crossing thread boundaries |
| `ThreadBackend` | Provides a concrete thread handle for explicit `fork`/`merge`, e.g. `pThread` |
| `Lent` | Enables `global: own`, a runtime-checked exclusive borrow. `global` bindings cannot be moved — `Lent` manages thread-safe distribution. Also allows `lend` parameters. |
| `Trust` | Zith-- only: does not remove the explicit `raw` marker required when calling a `raw fn`. |
| `Unique` | Marks a singleton type. It cannot be instantiated — the type name itself acts as the instance. All fields must implement `Share` (thread-safe). An `own Local` variant is a singleton thread-local. |

#### `Failable` & `Invalid` — Invalid-State Capabilities

`Failable` describes a type that can hold either a valid or an invalid state. Its contract
provides `check(): bool` to report whether the value is valid, `valid()` to represent its
valid state, and `invalid()` to represent its invalid state. Users do not call these contract
members directly. For `try x`, the compiler evaluates `x` once, calls `check()` once, then calls
exactly one of `valid()` or `invalid()` based on the check result.

```zith
// Config also implements Failable.
extend Config with Invalid {
    fn onUnavailable(self) { log("Config is unavailable"); }
}
```

`Invalid` is an optional capability for a type that implements `Failable`. It lets the type
provide methods for its invalid state. Those methods are callable only when flow analysis
proves the receiver invalid. For example, `cfg.onUnavailable()` is valid only in a branch
where flow analysis proves `cfg` invalid. An invalid state may be `Nil`, an error value, or a
state defined by the `Failable` type.

`Nil` is the universal absence state. It is invalid, but it is not an error. `Error` is a marker
capability, not a generic error type. The type of an invalid value implements `Error` to classify
that value as an error. `fail` captures only invalid values whose types implement `Error`, while
`catch` can capture any invalid value produced during `with` initialization. The exact declaration
syntax for a marker-only capability implementation remains open. `Null` and `Fail` are deprecated
capability names.

#### `Trust` — Safe Sections with Raw Code

> **Full-Zith direction:** the `trust fn` safety level supersedes this
> capability as the way safe code reaches raw and hazard work. See
> [ADR-0043](adr/0043-full-zith-safety-levels-safe-trust-hazard.md).
> In Zith--, `Trust` does not waive the explicit call-site marker.

```zith
trait Place
    extends Trust
{
    raw fn sample(): i32 {}
}

fn safeCaller(a: impl Place) {
    let v = raw a.sample();
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

#### `ThreadBackend` — Thread Fork/Merge

`ThreadBackend` is the runtime side of explicit thread fork/merge. A backend
object such as `pThread` creates a concrete `Thread<T>` handle, `merge` then
blocks and consumes that handle once:

```zith
let t: PThreadHandle<i32> = pThread fork Update(share state, n);
let result: i32 = merge t;
```

`fork` is a core keyword that names the entry action and the backend object,
`spawn Entry(args)` is a stdlib shorthand for the active backend. There is no
`await`, future, or resumable task in this protocol. The returned value is
exactly the result type declared by the entry action, including failable types
when the action can fail. See [the branch protocol plan](plans/branch-protocol.md)
for the full design.

### 4.5 Operator Overloading

Operator overloading is capability-based and strict. You implement only the operators you need:

```zith
extend Vec3 with Arithmetic {
    fn +(self, other: Self): Self { ... }
    fn -(self, other: Self): Self { ... }
}

extend Pipeline with Functor {
    fn (self, input: []u8): []u8 { ... }
}
let out = pipe(raw_bytes);
```

### 4.6 Trait Composition (`extends`)

`extends` composes one trait into another. Composition includes method requirements and defaults,
and carries capability identity. It does not inherit or embed fields. Structs declare their own
fields directly. A type that implements a composed trait satisfies the traits it composes without
counting their shared capabilities more than once.

```zith
trait Readable {
    fn read(self): i32;
}

trait BufferedReadable
    extends Readable
{
    fn bufferedRead(self): i32 { self.read(); }
}
```

When both clauses are present, write `extends` first and `requires` second,
each on its own line beneath the trait name.

If multiple composition paths reach the same original method or capability, that shared origin
counts once. Methods with the same exact signature are one requirement. Different defaults for
that signature conflict unless the composing trait resolves the conflict with `#[override]`.
That override replaces the inherited default. Its body is optional. Without a body, implementors
must provide the method. An override applies only to an inherited method with the same signature.

Composing a capability and one of its subtraits separately is a conflict. For example, if
`LogError extends Error`, a type must not implement both `Error` and `LogError` as separate
implementations. Implementing only `LogError` includes the `Error` capability once.
---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
