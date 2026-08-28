---
id: guide-types
title: Types
section: Language Guide
output: guide/D-types.html
aliases: language/D-types.html
kind: editorial
---
# Types

Zith has signed and unsigned integer types (`i8`-`i128`, `u8`-`u128`), floating-point types (`f32`, `f64`), `bool`, and `char`. Structs, components, enums, and unions are working core declarations. Arithmetic requires matching widths; there is no implicit promotion.

```zith
struct Point {
    x: i32,
    y: i32,
}

enum Direction { North, South, East, West }
```

Struct fields, struct literals, and field access all work end to end. Build a value with `Point { x: 1, y: 2 }`, read a field with `p.x`, take an address with `&p`, dereference with `*ptr`, and reach through a pointer with `ptr->x`.

```zith
fn shift(origin: Point): i32 {
    let moved: Point = Point { x: origin.x + 1, y: origin.y };
    let handle: *Point = &moved;
    handle->x
}
```

Arrays (`[N]T`), slices (`[]T`), and pointers (`*T`) are working, and indexing with `a[i]` lowers through HIR to code generation on all three.

## Pointers and opaque handles

`*T` is non-nullable: assigning `null` requires the optional pointer `?*T`. `*void` is rejected; use `raw opaque` for an untyped handle. A `raw opaque` casts to and from any `*T` with `as`, which is how you hold a pointer whose pointee type you do not want to name.

```zith
fn erase(p: *i32): raw opaque {
    p as raw opaque
}

fn restore(handle: raw opaque): *i32 {
    handle as *i32
}
```

Pointers imported from C headers arrive as `?*T` and are checked with `is null`. Flow-sensitive narrowing does not exist yet, so a `?*T` is still accepted unchecked where a `*T` is expected; check it anyway.

## Memory qualifiers

The subset enforces `lend` and `view`. They lower to pointer parameters and need explicit `lend x`/`view x` call annotations for `default` bindings; writing through `view` reports `E4004`. `mut`, `unique`, `share`, and `belong` are rejected with `E2010`.

```zith
fn peek(value: view i32): i32 {
    value
}
```

Layout builtins work in expression position: `@sizeOf(T)` types as `u64` for any complete type, and `@offsetOf(S, field)` and `@alignOf(S)` are struct-only and type as `i32`.

## Dynamic dispatch

`dyn Trait` and `dyn Interface` are implemented for method dispatch. A concrete value coerces to a fat pointer with a per-type vtable, and method calls through the value dispatch dynamically. Interface fields are used for conformance and are still readable on concrete types or generic bounds, but not through a `dyn Interface` value.

Start with explicit annotations when learning. The [Type System reference](doc:reference-03-type-system) defines generic types, unions, and the experimental narrowing rules; [Implementation Status](doc:reference-implementation-status) records what compiles today.
