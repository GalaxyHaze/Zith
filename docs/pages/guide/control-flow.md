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

`for` is the loop keyword. Three forms work today: the infinite loop `for { }`, the conditional loop `for (cond) { }`, and the 3-clause loop with parenthesized clauses `for (init), (cond), (step)` or the flat spelling `for (init, cond, step)`. `init` and `step` are both optional, and `continue` still runs the step before the next test.

```zith
fn sum(n: i32): i32 {
    var total: i32 = 0;
    for (var i: i32 = 0, i < n, i = i + 1) {
        total += i;
    }
    total
}
```

`while` still compiles, but it emits the deprecation warning `W1008` telling you to write `for (cond) { }` instead. The iterator form `for (x in xs)` is implemented through a duck-typed `next(self)` method returning a tagged union with two members: the element value and the canonical `End` marker.

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

## State machines and cleanup

`state`, `dock`, and `jump` are the implemented state-machine form. `state Name(params): ReturnType` declares a state, `dock Name(args)` starts one as an expression, and `jump Next(args)` is a terminating direct transfer. The old `flow fn`, `marker`, and `dock { ... }` block syntax is rejected. `defer expr;` and `defer { ... }` also work as reverse-order lexical cleanup. Consult the [Control Flow reference](doc:reference-09-control-flow) for the semantics and [Implementation Status](doc:reference-implementation-status) for the current boundary.
