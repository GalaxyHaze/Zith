---
id: guide-overview
title: Language Guide
section: Language Guide
output: zith--/guide/D-overview.html
aliases: language/D-overview.html
kind: editorial
---
# Zith-- Language Guide

This guide is the practical entry point to Zith--, the language built by the
`main` compiler and shipped as `zithc`. The [Zith-- reference](doc:zith-subset-reference)
describes its language contract. Zith has a separate language design in the
[Zith reference](doc:zith-overview).

## Learn the working language

Read these pages in order when learning the core:

- [Syntax](doc:guide-syntax) for expressions, operators, and literals.
- [Bindings](doc:guide-bindings) for `let`, `var`, and `const`.
- [Types](doc:guide-types) for structs, pointers, slices, and `dyn`.
- [Functions](doc:guide-functions) for declarations, overloads, and `raw fn`.
- [Control Flow](doc:guide-control-flow) for branches, loops, and `when`.
- [Modules](doc:guide-modules) for imports and visibility.

Then continue with [Generics](doc:guide-generics), [Traits & Interfaces](doc:guide-traits-interfaces),
and [C Interop](doc:guide-c-interop).

## Read boundaries before using advanced features

[Memory Model](doc:guide-memory-model), [Errors](doc:guide-errors),
[Comptime](doc:guide-comptime), [Contexts, Words & Macros](doc:guide-contexts-words-macros),
[Concurrency](doc:guide-concurrency), and [Raw & Unsafe](doc:guide-raw-unsafe)
include features with explicit implementation boundaries. Each page separates
what works in Zith-- from features that belong to Zith's separate design.

When an example needs a feature that is not implemented, the page links to the
formal reference instead of presenting that example as runnable code.
