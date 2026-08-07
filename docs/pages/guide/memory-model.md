---
id: guide-memory-model
title: Memory Model
section: Language Guide
output: guide/D-memory-model.html
aliases: language/D-memory.html, language/D-ownership.html
kind: editorial
---
# Memory Model

The specification defines Node Resource Analysis (NRA) and the memory qualifiers `mut`, `lend`, `view`, `unique`, `share`, and `belong`. NRA is no longer purely specification-only, but it is not finished either.

## What exists

The qualifiers parse anywhere a type is written and are carried in the type table as a qualified type. HIR and code generation strip the qualifier, so a qualified value has the same representation as an unqualified one. One rule is enforced today: writing through a `view` binding reports `E4004`.

```zith
fn peek(value: view i32): i32 {
    value
}
```

The pipeline boundary that ownership proof needs is also in place. The stable order is `sema -> comptime/solve -> NTA/NRA -> HIR`: semantic facts are accumulated and consumed before final lowering, and residual ownership facts attach to side tables rather than introducing ownership nodes into HIR.

## What is missing

The alive/dead/lent state machine and the rest of the ownership diagnostics (`E4001` use-after-move, `E4002` borrow conflict, `E4003` double borrow) are not implemented. Do not present ownership examples as compiler-enforced behaviour beyond the `view` write rule.

Read the [Memory Model reference](doc:reference-07-memory-model) for the design and [Implementation Status](doc:reference-implementation-status) for the boundary.
