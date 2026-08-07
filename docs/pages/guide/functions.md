---
id: guide-functions
title: Functions
section: Language Guide
output: guide/D-functions.html
aliases: language/D-functions.html
kind: editorial
---
# Functions

Declare a function with `fn`, typed parameters, and an optional return annotation. A final expression can provide the return value. The return type is written either `: R` or `-> R`; both spellings parse.

```zith
fn multiply(a: i32, b: i32): i32 {
    a * b
}

fn square(a: i32) -> i32 {
    a * a
}
```

## Overloading

Functions overload on parameter count and on parameter types. Overload resolution picks the matching declaration at the call site; an unresolvable call reports `E2007`, and an equally good pair reports `E2008`.

```zith
fn area(w: i32) -> i32 {
    w * w
}

fn area(w: i32, h: i32) -> i32 {
    w * h
}
```

Because overloads share a source name, linkage names are qualified as `<module>.<Owner>.<name>(<params>)`. The two exceptions are `extern fn` and `main`, which keep their plain C-visible names.

## Function kinds

`fn`, `flow fn`, `raw fn`, and `extern fn` parse and lower through HIR to code generation. `const fn` is still a parse error: `const` is a binding keyword, so `const fn f()` is read as a `const` binding named `fn` rather than a compile-time function.

See [Implementation Status](doc:reference-implementation-status) before relying on the other forms, and [Concurrency](doc:guide-concurrency) for the status of the legacy `async fn` spelling.
