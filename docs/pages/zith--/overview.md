---
id: zith-subset-overview
title: Zith-- Overview
section: Zith--
output: zith--/D-overview.html
aliases: language/D-zith-subset.html
kind: editorial
---
# Zith-- Overview

Zith-- is a statically typed systems language with its own specification and
language contract. It is a sibling of Zith, not a subset or an implementation
stage of that language. The `main` compiler builds Zith-- and ships as
`zithc`. Use this section to write programs supported by the current compiler.

Zith-- is in beta: its current language surface is near-complete and its
behavior is intended to remain stable. Planned work focuses on additive
language and standard-library extensions. See
[Implementation Status](doc:reference-implementation-status) for verified
compiler behavior and known restrictions.

## What works today

The implemented language includes:

- typed functions and no-capture function literals; structs, enums, unions,
  arrays, slices, pointers, and optionals;
- `let`, `var`, and `const` bindings;
- `if`, `for`, `when`, `defer`, and state-machine control flow;
- modules, aliases, visibility, generic functions and types;
- nominal traits, structural interfaces, and method dispatch through `dyn`;
- normal macros and `raw macro`;
- `extern fn`, C header imports, and `raw opaque` handles;
- `lend` and `view` parameters with call-site ownership annotations;
- HIR lowering, cache serialization, LLVM code generation, and WebAssembly
  execution.

The [Implementation Status](doc:reference-implementation-status) page is the
source of truth when a feature has partial support or a known restriction.

## Separate language contracts

Zith has a separate language-design specification. Its comptime evaluation,
NRA ownership model, contexts, words, tag items, `unsafe` hierarchy, and error
model do not define Zith-- behavior. Their presence in the Zith reference does
not mean they are planned for Zith--.

The compiler reports unsupported syntax instead of silently accepting a
feature with different semantics. Read the [Zith reference](doc:zith-overview)
for the Zith design, and use this section and [Implementation
Status](doc:reference-implementation-status) for the behavior of Zith--.

## A small program

```zith
struct Point {
    x: i32,
    y: i32,
}

fn distanceOnX(left: Point, right: Point): i32 {
    right.x - left.x
}

fn main(): i32 {
    let origin = Point { x: 0, y: 10 };
    let target = Point { x: 42, y: 10 };
    distanceOnX(origin, target)
}
```

Continue with the [Zith-- Language Guide](doc:guide-overview), or use the
[English Zith-- reference](doc:zith-subset-reference) for the language contract.
