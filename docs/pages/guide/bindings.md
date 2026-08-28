---
id: guide-bindings
title: Bindings
section: Language Guide
output: guide/D-bindings.html
aliases: language/D-variables.html, language/D-packs.html
kind: editorial
---
# Bindings

Use `let` for an immutable binding and `var` when the binding will be reassigned. `const` is for an immutable global or local value in `Zith--`; `global` is not supported and reports an error suggesting `const`.

```zith
let answer: i32 = 42;
var count: i32 = 0;
count = count + 1;
```

Be aware that `const` currently means immutable, not compile-time evaluated. There is no comptime evaluation yet, so do not read `const` as a request to fold a value during compilation. A `const` initializer must be a constant expression: literals, aggregates of literals, `const` struct fields, and references to earlier `const` values. Function calls are not constant expressions.

Parameters are immutable by default. `var p: T` makes a parameter mutable; `var self` allows in-place mutation of receiver fields.

Destructuring and the full mutability model are specified in the [Bindings reference](doc:reference-06-mutability-bindings).
