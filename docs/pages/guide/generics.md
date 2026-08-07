---
id: guide-generics
title: Generics
section: Language Guide
output: guide/D-generics.html
aliases: language/D-generics.html
kind: editorial
---
# Generics

Generic parameter lists use angle brackets and are accepted on `fn`, `struct`, `type` alias, `enum`, `union`, and `trait` declarations. Inside the declaration, each parameter resolves as an opaque type.

```zith
struct Pair<T, U> {
    first: T,
    second: U,
}

fn identity<T>(value: T): T {
    value
}
```

Both declarations above pass `zithc check`. Instantiation does not: calling `identity<i32>(42)`, or relying on inference with `identity(42)`, fails in the comptime solver with `E3001` — "generic parameter T has no concrete type". Generic code therefore does not compile end to end yet.

Constraints of the form `T: Trait` parse but are not enforced. The complete syntax is in the [Type System reference](doc:reference-03-type-system); the boundary is in [Implementation Status](doc:reference-implementation-status).
