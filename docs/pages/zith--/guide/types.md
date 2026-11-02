---
id: guide-types
title: Types
section: Language Guide
output: zith--/guide/D-types.html
aliases: language/D-types.html
kind: editorial
---
# Types

Zith-- has signed and unsigned integer types (`i8`-`i128`, `u8`-`u128`), floating-point types (`f32`, `f64`), `bool`, and `char`. Structs, components, enums, and unions are working core declarations. Arithmetic requires matching widths; there is no implicit promotion.

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

Arrays (`[N]T`), slices (`[]T`), pointers (`*T`), and packs are working.
Array and slice indexing uses `a[i]`. Pack indexing uses a compile-time
integer position.

## Packs, the tuple-like type

In Zith--, a tuple-like aggregate is called a **pack**. It can hold values
with different types and preserves their order:

```zith
let point = | 10, 20, 'x' |;

let x: i32 = point[0];
let y: i32 = point[1];
let flag: char = point[2];
```

Use binding destructuring when each position needs a local name:

```zith
let [x, y, flag] = point;
```

Pack positions are known at compile time. A pack is not a homogeneous array,
so use `[N]T` or `[]T` when the program needs runtime indexing over values of
one type. Named pack types can describe the member types and names explicitly:

```zith
fn sample(): |x: i32, y: i32, flag: char| {
    |10, 20, 'x'|
}
```

Packs are useful for returning several heterogeneous values, destructuring
those values at the call site, and carrying multiple values through a loop
accumulator. The current implementation supports positional literals,
compile-time indexing, and `[a, b, c]` binding destructuring. It does not
provide tuple methods or runtime variable indexing.

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

Pointers imported from C headers arrive as `?*T`. An early-return guard using
`is null` narrows a local optional pointer before dereferencing it. Proof is
local: derived expressions and cross-function checks are not tracked, and an
unchecked nullable pointer where `*T` is required reports `E3005`.

## Memory qualifiers

Zith-- enforces `lend` and `view`. They lower to pointer parameters and need explicit `lend x`/`view x` call annotations for `default` bindings; writing through `view` reports `E4004`. `mut`, `unique`, `share`, and `belong` are rejected with `E2010`.

```zith
fn peek(value: view i32): i32 {
    value
}
```

Layout builtins work in expression position: `@sizeOf(T)` types as `u64` and
`@alignOf(T)` as `i32` for any complete type. `@offsetOf(S, field)` is
struct-only and types as `i32`.

## Dynamic dispatch

`dyn Trait` and `dyn Interface` are implemented for method dispatch. A concrete value coerces to a fat pointer with a per-type vtable, and method calls through the value dispatch dynamically. Interface fields are used for conformance and are still readable on concrete types or generic bounds, but not through a `dyn Interface` value.

Start with explicit annotations when learning. The [Type System reference](doc:reference-03-type-system)
describes Zith's type design and preserves some legacy Zith-- spellings in its
examples. The [Zith-- reference](doc:zith-subset-reference) defines this
language's contract; [Implementation Status](doc:reference-implementation-status)
records compiler support.
