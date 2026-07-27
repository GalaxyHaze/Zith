---
id: guide-functions
title: Functions
section: Language Guide
output: guide/D-functions.html
aliases: language/D-functions.html
kind: editorial
---
# Functions

Declare a function with `fn`, typed parameters, and an optional return annotation. A final expression can provide the return value.

```zith
fn multiply(a: i32, b: i32): i32 {
    a * b
}
```

`fn`, `flow fn`, `const fn`, and `raw fn` parse and lower. Compile-time evaluation for `const fn` remains specification-only, so use ordinary `fn` for code that must run today. See [Implementation Status](doc:reference-implementation-status).
