## 14. Runtime: Polymorphism & Dynamic Behaviour

> **Implementation status:** Static dispatch via generics and `implement T as Trait {}` is
> **working**. `dyn Trait` dynamic dispatch is **spec-only** — `dyn` is a parse error in type
> position. Vtable layout and runtime dispatch are not implemented.
> See [impl-status.md](impl-status.md).

### 14.1 Static vs Dynamic Dispatch

By default, Zith uses static dispatch — the compiler knows the exact implementation at compile time. Zero overhead.

Use `dyn` for dynamic dispatch. At the call site you get polymorphism; the compiler and LLVM can often optimize away the indirection, making it zero-cost in practice.

### 14.2 `dyn` as a Type Hint

Like `union` (see [§3.6](03-type-system.md#36-union)), `dyn` works as a **type hint**. When the compiler can't deduce the concrete type, you write `dyn` to tell it you want dynamic behavior:

```zith
let x: dyn = 5;
if (x is i32) x += 32;   // smart-cast inside the branch

// dyn []T — dynamic Trait slice
let items: dyn []Drawable = shapes;
// dyn slice
let dynList: dyn [];

// In many cases LLVM strips the dyn overhead entirely
// even if not, compare the type is a simple int comparison
// in terms of performance, is union + ptr indirection, still fast
```

Inside a smart-cast branch (`is`), the type narrows to the concrete type. Mutations inside the branch affect the inner value; outside, assigning to the variable changes what the `dyn` points to (if `var`).

### 14.3 `dyn` Traits

`dyn Trait` is a `view` by default — a read-only, non-owning reference with a vtable. That means `view dyn` is redundant.

All other memory modifiers work with `dyn`:

| Keyword | `dyn` behavior |
|---|---|
| `view dyn` | Redundant — `dyn` is already a view |
| `share dyn` | Multiple names, same dynamic value |
| `lend dyn` | Exclusive mutable borrow of a dynamic value |
| `unique dyn` | Single-owner dynamic value |

```zith
fn draw_all(items: dyn []Drawable) {
    for (item in items) { item.render(); }
}

//specific verbose, you could use an interface or alias to simplify
fn modify(shape: lend dyn Drawable) {
    shape.scale(2.0);
}
```

`dyn` does **not** work with `interface` (see [§4.3](04-traits-interfaces.md#43-capabilities)) — only traits. Interfaces are structural and don't carry a vtable.

When you write a type that could be `dyn` or `opaque`, prefer `dyn` — it's short and clearer. Reserve `opaque` for cases where you specifically need `raw opaque` (untagged `void*`, C interop).

### 14.4 `dyn` vs Generics

| | Generics | `dyn` |
|---|---|---|
| Dispatch | Static — one copy per type | Dynamic — single code path |
| Code size | Larger (N copies) | Smaller (one copy) |
| Performance | No indirection | Vtable indirection (often elided by LLVM) |
| When to use | Hot loops, known types at compile time | Heterogeneous collections, plugins |

```zith
// Generic — compiler monomorphizes
fn log<T: Printable>(val: T) { val.print(); }

// dyn — vtable dispatch, LLVM may inline away the overhead
fn logDyn(val: dyn Printable) { val.print(); }
```

### 14.5 Object Safety

A trait is object-safe if all its methods meet these rules:

- No `Self` in parameter or return types (except `self`, `other`)
- No generic type parameters on the method
- No `Self: Sized` requirements

If you try to use a non-object-safe trait with `dyn`, the compiler rejects it.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
