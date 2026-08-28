---
id: guide-generics
title: Generics
section: Language Guide
output: guide/D-generics.html
aliases: language/D-generics.html
kind: editorial
---
# Generics

Generic parameter lists use angle brackets and are accepted on `fn`, `struct`, `type` alias, `enum`, `union`, and `trait` declarations. Generic functions, structs, aliases, enums, unions, and `implement` blocks are monomorphized before HIR; explicit and inferred calls work.

```zith
struct Pair<T, U> {
    first: T,
    second: U,
}

fn identity<T>(value: T): T {
    value
}
```

Both declarations compile, and `identity<i32>(42)` plus inference `identity(42)` type-check and lower.

Constraints of the form `T: A + B` are parsed, stored, and enforced at generic call sites; an argument that does not satisfy a trait bound reports `E3009`, and an interface bound reports `E2024`. Interface bounds expose their fields and methods to the generic body. The complete syntax is in the [Type System reference](doc:reference-03-type-system); the boundary is in [Implementation Status](doc:reference-implementation-status).
