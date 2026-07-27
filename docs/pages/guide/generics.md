---
id: guide-generics
title: Generics
section: Language Guide
output: guide/D-generics.html
aliases: language/D-generics.html
kind: editorial
---
# Generics

Generic declarations use angle brackets. Simple monomorphization is working.

```zith
struct Pair<T, U> {
    first: T,
    second: U,
}
```

Complex constraints are only partially implemented. Keep early code to simple type parameters and verify the result with `zithc check`. The complete syntax is in the [Type System reference](doc:reference-03-type-system).
