---
id: guide-comptime
title: Comptime
section: Language Guide
output: zith--/guide/D-comptime.html
aliases: language/D-intrinsics.html
kind: editorial
---
# Comptime

The Zith design specifies `const` blocks, compile-time functions, reflection,
and type transformation. These features are not part of Zith--, which does
not evaluate arbitrary code at compile time.
Treat `const` as an immutability keyword, not as a request for compile-time
evaluation.

Layout intrinsics are the exception. `@` parses in expression position and the layout builtins work:

```zith
struct Point {
    x: i32,
    y: i32,
}

fn stride(): u64 {
    @sizeOf(Point)
}
```

`@sizeOf(T)` accepts any complete type and types as `u64`; `@sizeOf(void)` reports `E3001`. `@alignOf(T)` accepts any complete type, including primitives and generic `T`, and types as `i32`. `@offsetOf(S, field)` remains struct-only and also types as `i32`.

## What `const` means

This is a valid immutable binding:

```zith
const answer: i32 = 42;
```

It is not a `const fn` call and it does not cause arbitrary function execution
during compilation. A `const` initializer must use values accepted by the
current constant-expression rules. Function calls are not constant
expressions.

## Zith's separate comptime design

Comptime evaluation, `const fn`, reflection helpers such as `@appendField`,
`@removeField`, and `@appendMethod`, and type transformation remain
outside the Zith-- contract. Read the [Comptime reference](doc:reference-11-comptime)
for the Zith design and [Implementation Status](doc:reference-implementation-status)
for the Zith-- compiler boundary.
