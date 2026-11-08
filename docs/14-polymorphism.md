## 14. Runtime: Polymorphism & Dynamic Behaviour

> **Status:** see [impl-status.md](impl-status.md) for the current status of static and dynamic
> dispatch. Zith-- supports `dyn Trait` and `dyn Interface` method dispatch: concrete values are
> coerced to a fat pointer (data pointer plus a method vtable), and calls on the `dyn` value are
> lowered through that vtable. Interface fields remain available on concrete types and generic
> bounds, not on `dyn` values.

### 14.1 Static vs Dynamic Dispatch

By default, Zith uses static dispatch. The compiler knows the exact implementation at compile time, so dispatch costs zero overhead.

Use `dyn` for dynamic dispatch. At the call site you get polymorphism. The compiler and LLVM can often optimize away the indirection, making it zero-cost in practice.

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

Inside a smart-cast branch (`is`), the type narrows to the concrete type. Mutations inside the branch affect the inner value. Outside, assigning to the variable changes what the `dyn` points to (if `var`).

### 14.3 `dyn` Traits

A bare `dyn Trait` is the boxed trait object, `own dyn Trait`, not an implicit
read reference. The transfer mode is written explicitly:

| Spelling | `dyn` behavior |
|---|---|
| `&dyn Trait` | Read reference to a dynamic value. |
| `lend dyn Trait` | Exclusive mutable reference to a dynamic value. |
| `own dyn Trait` | Single-owner dynamic value (boxed trait object). The vtable carries the drop, so the box is a known guardian. A bare `dyn Trait` means this. |
| `dyn T` (concrete `T`) | A dynamic projection. It erases the outer storage and has no guardian, so a bare `dyn T` is an error and `own dyn T` is rejected. Write the reference, for example `&dyn T` or `lend dyn T`. See [ADR-0056](adr/0056-nra-erased-forms-and-spelling.md). |

```zith
fn draw_all(items: dyn []Drawable) {
    for (item in items) { item.render(); }
}

//specific verbose, you could use an interface or alias to simplify
fn modify(shape: lend dyn Drawable) {
    shape.scale(2.0);
}
```

Zith-- supports dynamic dispatch for both nominal traits and structural interfaces.
Vtables are generated per `(trait/interface, concrete type)` pairing. For a nominal
trait, `implement Owner as Trait` methods (or trait defaults) fill the slots. For an
interface, the concrete owner methods fill the slots.

This describes Zith-- only. In the full-Zith specification, an interface is a static
contract rather than a dynamic-dispatch type, so `dyn Interface` is not part of that model.

The Zith-- `main` now supports `dyn Interface`, with one deliberate difference
from the future spec beyond this file: the public surface exposes only methods.
The fat pointer carries a concrete data pointer plus a vtable whose slots point
to the concrete owner methods. Interface fields are used for conformance and
are still readable through concrete types or generic bounds, but not through a
`dyn Interface` value. `a.x` is rejected with `error[E3001]`.

When you write a type that could be `dyn` or `opaque`, prefer `dyn`, because it is short and clearer. Reserve `opaque` for cases where you specifically need `raw opaque` (untagged `void*`, C interop).

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
