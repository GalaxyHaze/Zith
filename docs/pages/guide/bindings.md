---
id: guide-bindings
title: Bindings
section: Language Guide
output: guide/D-bindings.html
aliases: language/D-variables.html, language/D-packs.html
kind: editorial
---
# Bindings

Use `let` for an immutable binding and `var` when the binding will be reassigned. `const` and `global` are also part of the language, and all four forms work in the current compiler.

```zith
let answer: i32 = 42;
var count: i32 = 0;
count = count + 1;
```

Be aware that `const` currently means immutable, not compile-time evaluated. There is no comptime evaluation yet, so do not read `const` as a request to fold a value during compilation.

Destructuring and the full mutability model are specified in the [Bindings reference](doc:reference-06-mutability-bindings).
