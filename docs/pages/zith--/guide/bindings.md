---
id: guide-bindings
title: Bindings
section: Language Guide
output: zith--/guide/D-bindings.html
aliases: language/D-variables.html, language/D-packs.html
kind: editorial
---
# Bindings

Use `let` for an immutable binding and `var` when the binding will be reassigned.
`const` declares immutable global or local values in the Zith-- contract.
`global name: T = value` is specified as rejected with an `E2010` suggestion
to use `const`. The imported implementation-status table currently lists a
`global` binding form as working, which conflicts with the specification and
implementation notes; use `const` as the canonical spelling.

```zith
let answer: i32 = 42;
var count: i32 = 0;
count = count + 1;
```

Be aware that `const` currently means immutable, not compile-time evaluated. There is no comptime evaluation yet, so do not read `const` as a request to fold a value during compilation. A `const` initializer must be a constant expression: literals, aggregates of literals, `const` struct fields, and references to earlier `const` values. Function calls are not constant expressions.

Parameters are immutable by default. `var p: T` makes a parameter mutable; `var self` allows in-place mutation of receiver fields.

## Destructure a pack

In Zith--, packs are tuple-like aggregates. Use `| ... |` to create one and
`[ ... ]` to bind its members by position:

```zith
let measurement = | 42, 3.5f, 'C' |;
let [value, scale, unit] = measurement;
```

The pattern must match the pack's member count. Use `pack[index]` when one
position is needed without binding the others. The index must be a compile-time
integer.

Destructuring is part of the Zith-- contract. The [Bindings reference](doc:reference-06-mutability-bindings)
describes Zith's separate, deeper mutability design.
