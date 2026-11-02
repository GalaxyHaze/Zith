---
id: guide-functions
title: Functions
section: Language Guide
output: zith--/guide/D-functions.html
aliases: language/D-functions.html
kind: editorial
---
# Functions

Declare a function with `fn`, typed parameters, and an optional return
annotation. A final expression can provide the return value. Zith-- accepts
both `: ReturnType` and `-> ReturnType` for function declarations, although
the colon form is the canonical spelling used in this guide.

```zith
fn multiply(a: i32, b: i32): i32 {
    a * b
}

fn square(a: i32): i32 {
    a * a
}

fn cube(a: i32) -> i32 {
    a * a * a
}
```

## Overloading

Functions overload on parameter count and on parameter types. Overload
resolution picks the matching declaration at the call site. An unresolvable
call reports `E2007`, and an equally good pair reports `E2008`.

```zith
fn area(w: i32): i32 {
    w * w
}

fn area(w: i32, h: i32): i32 {
    w * h
}
```

Because overloads share a source name, linkage names are qualified as `<module>.<Owner>.<name>(<params>)`. The two exceptions are `extern fn` and `main`, which keep their plain C-visible names.

## Function values and function pointers

Function types use the colon spelling:

```zith
fn double(value: i32): i32 {
    value * 2
}

extern fn apply(operation: fn(i32): i32, value: i32): i32

fn main(): i32 {
    var operation: fn(i32): i32 = double;
    apply(operation, 21)
}
```

Function references, parameters with a function type, and calls through those
values are implemented. They lower to C-compatible function pointers, which
makes them useful for callbacks and C interop.

Function literals are also supported without captures:

```zith
fn apply(operation: fn(i32): i32, value: i32): i32 {
    operation(value)
}

fn main(): i32 {
    apply(fn (value: i32): i32 { value * 2 }, 21)
}
```

These literals are plain function values. They cannot refer to locals from an
enclosing function; capturing closures are outside the Zith-- contract.

## Function kinds

`fn`, `raw fn`, and `extern fn` parse and lower through HIR to code generation.
`state` is a separate function kind for stackless state machines with `dock`
and `jump`. `const fn` is rejected with `E2010`, and the old `flow fn` syntax
is rejected. `const` is a binding keyword, not a function qualifier.

See [Implementation Status](doc:reference-implementation-status) before relying on the other forms, and [Concurrency](doc:guide-concurrency) for the status of the legacy `async fn` spelling.
