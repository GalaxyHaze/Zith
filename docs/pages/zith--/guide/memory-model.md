---
id: guide-memory-model
title: Memory Model
section: Language Guide
output: zith--/guide/D-memory-model.html
aliases: language/D-memory.html, language/D-ownership.html
kind: editorial
---
# Memory Model

Zith and Zith-- define different memory and ownership contracts. The Zith
design uses Node Resource Analysis (NRA) and the qualifiers `mut`, `lend`,
`view`, `unique`, `share`, and `belong`. Zith-- has its own contract and
currently supports `lend` and `view` parameters with call-site annotations.

## Borrowing with `lend` and `view`

`lend T` and `view T` parameters lower to pointers. A `default` binding passed
to a `lend` or `view` parameter requires `lend x` or `view x` at the call site.
Omitting the annotation or using the wrong mode reports `E4005`. The same
binding cannot be lent twice or used as both `lend` and `view` in one call.

Use `view` when the callee only needs to read:

```zith
fn peek(value: view i32): i32 {
    value
}
```

Use `lend` when the callee may write through the reference:

```zith
struct Counter {
    value: i32,
}

fn increment(counter: lend Counter) {
    counter.value = counter.value + 1;
}
```

Bindings with the default ownership mode need an explicit annotation at the
call site:

```zith
var counter = Counter { value: 0 };
increment(lend counter);
```

The annotation states the borrow mode at the boundary. It does not transfer
ownership into the callee.

## Pointers and address-taking

`*T` is a non-nullable pointer object. `?*T` is its nullable form. Taking an
address creates a pointer to the local storage:

```zith
fn readPoint(): i32 {
    var value: i32 = 10;
    let pointer: *i32 = &value;
    *pointer
}
```

The compiler tracks local pointer aliases and rejects pointers that escape
their valid scope. Returning a pointer to a local, storing it in a persistent
aggregate, or placing it in a global is not a substitute for allocating
storage.

## `view` is read-only

A `view` parameter can read its referent but cannot write through it:

```zith
fn readOnly(counter: view Counter): i32 {
    counter.value
}
```

Writing through a `view` is rejected. Use `lend` when the function needs write
access. LLVM emits `nocapture` for borrows and `readonly` for views.

## What is missing

Zith-- does not implement Zith's full alive/dead/lent ownership proof
state machine. Its smaller SRA does check local borrow conflicts (`E4002`)
and double borrows (`E4003`). `E4001` is emitted for logical receiver moves
after calls that consume a method receiver. `unique`, `share`, `belong`, and
`mut` are rejected with `E2010` in Zith--.

The ownership pipeline boundary is in place. The stable order is
`sema -> comptime/solve -> NTA/SRA -> HIR`. Semantic facts are accumulated and
consumed before final lowering, and residual ownership facts attach to side
tables instead of introducing ownership nodes into HIR.

Read the [Memory Model reference](doc:reference-07-memory-model) for the Zith
design and [Implementation Status](doc:reference-implementation-status) for
the Zith-- compiler behavior. The Zith model is not a roadmap for Zith--.
