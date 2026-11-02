---
id: guide-errors
title: Errors
section: Language Guide
output: zith--/guide/D-errors.html
aliases: language/D-errors.html
kind: editorial
---
# Optional Values and Errors

Optional (`?T`) types are a working declared type. `null` and plain `T` values
coerce to `?T`, and the postfix `?` operator propagates an optional
operand inside a function that itself returns an optional. Failable `T!` types
are not part of Zith--.

```zith
fn halve(value: ?i32): ?i32 {
    let inner: i32 = value?;
    inner / 2
}
```

The recovery vocabulary around those types is not implemented in Zith--.
`fail`, `with`, `catch`, and `throw` belong to Zith's separate error model;
their presence there does not make them planned Zith-- features. Zith--
supports `must value` to extract an optional payload, panicking if it is
`null`. Keep examples to optionals and explicit branching.

`as` is a real cast that lowers to an LLVM conversion, covering numeric pairs and `raw opaque` <-> `*T`; other conversions are rejected and there is no narrowing overflow check. `is null` requires an optional operand, and `is Type` is implemented for tagged-union members and `opaque`. An early-return `is null` guard narrows a local `?*T` or optional aggregate, including `?S`, `?pack`, and `?i32`. Proof for derived expressions and across function boundaries is not tracked; without a local proof, `E3005` reports an unproven nullable use.

The Zith error model is described in the [Error Handling reference](doc:reference-08-error-handling); current Zith-- compiler behavior is in [Implementation Status](doc:reference-implementation-status).

## Check before extracting

The clearest Zith-- pattern is an explicit branch:

```zith
fn portOrDefault(port: ?i32): i32 {
    if (port) {
        return raw port;
    }

    return 8080;
}
```

Use `raw value` only when another invariant proves that the optional contains
a value. It skips the normal check and is documented in [Raw & Unsafe](doc:guide-raw-unsafe).

## Zith's separate error model

Zith specifies failable values, propagation, recovery, and explicit failure.
These are Zith language features, not an incomplete Zith-- workflow. Do not
use examples from the Zith reference as Zith-- programs.
