---
id: guide-memory-model
title: Memory Model
section: Language Guide
output: guide/D-memory-model.html
aliases: language/D-memory.html, language/D-ownership.html
kind: editorial
---
# Memory Model

The specification defines Node Resource Analysis (NRA) and the memory qualifiers `mut`, `lend`, `view`, `unique`, `share`, and `belong`. `Zith--` implements a simplified reference-analysis slice around `lend` and `view`; the full NRA proof is not finished.

## What exists

`lend T` and `view T` parameters lower to pointers. A `default` binding passed to a `lend`/`view` parameter requires `lend x` or `view x` at the call site; omitting it or passing the wrong annotation reports `E4005`. The same binding cannot be lent twice or used as both `lend` and `view` in one call. Writing through a `view` binding reports `E4004`; writing through `lend` is allowed. LLVM emits `nocapture` for borrows and `readonly` for views.

```zith
fn peek(value: view i32): i32 {
    value
}
```

The pipeline boundary that ownership proof needs is also in place. The stable order is `sema -> comptime/solve -> NTA/NRA -> HIR`: semantic facts are accumulated and consumed before final lowering, and residual ownership facts attach to side tables rather than introducing ownership nodes into HIR.

## What is missing

The alive/dead/lent state machine and the rest of the ownership diagnostics (`E4002` borrow conflict, `E4003` double borrow) are not implemented. `E4001` is emitted for logical receiver moves after calls that consume a method receiver. `unique`, `share`, `belong`, and `mut` are rejected with `E2010` in this subset.

Read the [Memory Model reference](doc:reference-07-memory-model) for the design and [Implementation Status](doc:reference-implementation-status) for the boundary.
