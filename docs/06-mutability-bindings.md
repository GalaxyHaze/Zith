## 6. Mutability & Bindings

> **Implementation status:** `let`, `var`, `const`, and `global` bindings are **working**. Binding
> destructuring with `[ ]` and pack literals are **spec-only** — no parser or sema support.
> NRA ownership modifiers (`lend`, `view`, `unique`, `share`, `belong`) are **spec-only**.
> See [impl-status.md](impl-status.md).

### 6.1 Deep Mutability Model

Zith uses deep mutability: a modifier on a binding flows into every nested field. Fields inside a struct inherit the mutability of the instance that holds them — no per-field `mut` annotation needed.

### 6.2 Binding Keywords

| Keyword | Controls | Semantics |
|---|---|---|
| `let` | Binding | Immutable — cannot be reassigned. |
| `var` | Binding | Mutable — can be reassigned. |
| `global` | Binding | Static storage duration. |
| `const` | Binding | Compile-time constant. |


> `let`/`var` control reassignability of the binding itself. Content mutability is handled separately through memory modifiers ([§7](07-memory-model.md)).

```zith
// let/var control REBIND only. Content mutability comes from memory modifiers.
let x: mut Point;      // cannot reassign x; Point's fields are mutable (mut)
var y: Point;          // can reassign y; Point's fields are immutable (default, no mut)

// lend, unique, share, belong → imply mut
fn update(p: lend Point) { p.x += 1; }  // p is mutable (lend implies mut)
let r: unique Resource = acquire();     // r's fields are mutable (unique implies mut)

// view → implies immutable
fn read(c: view Config) { ... }         // c is read-only (view implies immutable)

const PI = 3.14159;
const COUNT: mut = 0;
COUNT += 1;   // valid at compile time only
```

### 6.3 Destructuring

```zith
let [x, y, z]: f32 = 1.0f;             // grouped same type, related semantics
let name: string; let age: i32; // individual unrelated
let [x,y,z] = | 5,4,'c'|;   // pack literal — see [§6.4](#64-pack-literals)

// If the loop never runs, 'or' supplies the fallback value
let r = for ([acc, i]: i32), (i in 0..n) {
            acc *= i + 1
        } or 0;
```

### 6.4 Pack Literals

Packs group heterogeneous values into a lightweight tuple-like structure. They are declared with `| |` and can be destructured with `[ ]`:

```zith
// Pack literal
let p = | 5, 4, 'c' |;

// Destructure
let [a, b, c] = p;

// Used in for loops with type annotation
let r = for ([acc, i]: i32), (i in 0..n) {
            acc *= i + 1
        } or 0;
```

> Packs are like anonymous structs — the compiler extracts fields by order and passes them as function arguments. They have a concrete layout determined at compile time. They are primarily used for destructuring and as loop accumulators.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
