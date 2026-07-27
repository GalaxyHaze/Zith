---
id: guide-bindings
title: Bindings
section: Language Guide
output: guide/D-bindings.html
aliases: language/D-variables.html, language/D-packs.html
kind: editorial
---
# Bindings

Use `let` for an immutable binding and `var` when the binding will be reassigned. `const` and `global` are also part of the language, with separate compile-time and storage semantics.

```zith
let answer: i32 = 42;
var count: i32 = 0;
count = count + 1;
```

The compiler supports these binding forms. Destructuring and the full mutability model are specified in [Bindings reference](doc:reference-06-mutability-bindings).
