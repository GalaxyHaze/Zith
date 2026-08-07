---
id: guide-errors
title: Errors
section: Language Guide
output: guide/D-errors.html
aliases: language/D-errors.html
kind: editorial
---
# Errors

Optional (`?T`) and failable (`T!`) types are working declared types. `null` coerces to `?T`, a plain `T` coerces to `?T`, and the postfix `?` operator propagates an optional operand inside a function that itself returns an optional.

```zith
fn halve(value: ?i32): ?i32 {
    let inner: i32 = value?;
    inner / 2
}
```

The recovery vocabulary around those types is not implemented. `fail`, `with`, `catch`, `must`, and `throw` remain specification-only. Keep examples to optionals and explicit branching until error values land.

`as` is a real cast that lowers to an LLVM conversion, covering numeric pairs and `raw opaque` <-> `*T`; other conversions are rejected and there is no narrowing overflow check. `is` supports only `is null`, which requires an optional operand; any other operand reports a dedicated diagnostic. Flow-sensitive narrowing after an `is null` check does not exist yet, so `E3005` is reported where non-null proof is required.

The planned rules are in the [Error Handling reference](doc:reference-08-error-handling); the current boundary is in [Implementation Status](doc:reference-implementation-status).
