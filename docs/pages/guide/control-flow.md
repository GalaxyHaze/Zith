---
id: guide-control-flow
title: Control Flow
section: Language Guide
output: guide/D-control-flow.html
aliases: language/D-control-flow.html
kind: editorial
---
# Control Flow

`if`, `else if`, and `else` are working control-flow constructs, together with `break`, `continue`, and `return`.

```zith
fn sign(value: i32): i32 {
    if (value < 0) {
        -1
    } else {
        1
    }
}
```

## Loops

`for` is the loop keyword. Three forms work today: the infinite loop `for { }`, the conditional loop `for (cond) { }`, and the 3-clause loop `for (init; cond; step) { }` with semicolon separators. `init` and `step` are both optional, and `continue` still runs the step before the next test.

```zith
fn sum(n: i32): i32 {
    var total: i32 = 0;
    for (var i: i32 = 0; i < n; i = i + 1) {
        total += i;
    }
    total
}
```

`while` still compiles, but it emits the deprecation warning `W1008` telling you to write `for (cond) { }` instead. The iterator form `for (x in xs)` is not implemented and reports a parse error.

## Pattern matching

`when` matches a value against comma-separated arms written `(pattern) ~> body`. `match` is a synonym for the same construct. Equality, boolean, and range patterns lower through HIR to code generation.

```zith
fn classify(x: i32): i32 {
    when (x) {
        (0) ~> 100,
        (1..3) ~> 200,
        (_) ~> 300,
    }
}
```

`(_)` is the default arm and must be written last. A `when` used for its value without a default is rejected as non-exhaustive.

## Not yet available

`marker` and `jump` work as a block-style go-to inside `flow fn`; `dock` is still a parse error. Consult the [Control Flow reference](doc:reference-09-control-flow) for the intended semantics and [Implementation Status](doc:reference-implementation-status) for the current boundary.
