---
id: guide-comptime
title: Comptime
section: Language Guide
output: guide/D-comptime.html
aliases: language/D-intrinsics.html
kind: editorial
---
# Comptime

Zith specifies `const` blocks, compile-time functions, reflection, and type transformation. None of that is evaluated today: comptime evaluation and `const fn` evaluation are both specification-only, and the reflection helpers `@appendField`, `@removeField`, and `@appendMethod` have no implementation. In `Zith--`, `const fn` is rejected with `E2010`.

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

`@sizeOf(T)` accepts any complete type and types as `u64`; `@sizeOf(void)` reports `E3001`. `@offsetOf(S, field)` and `@alignOf(S)` are struct-only and type as `i32`.

Note that `const` in Zith means immutable, not compile-time evaluated. Read the [Comptime reference](doc:reference-11-comptime) for the intended model and [Implementation Status](doc:reference-implementation-status) for the boundary.
