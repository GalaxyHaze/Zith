---
id: faq-overview
title: FAQ
section: FAQ
output: faq/D-overview.html
aliases: faq/D-overview.html, faq/D-philosophy.html, faq/D-security.html, faq/D-rust-comparison.html, faq/D-use-cases.html
kind: editorial
---
# FAQ

## Is Zith stable?

No. Zith language documentation is draft and experimental. The `main` compiler builds the `Zith--` subset documented in the [Zith-- reference](doc:reference-zith-subset); the [Implementation Status](doc:reference-implementation-status) page separates working compiler functionality from check-only, parse-skipped, parse-error, stub, and specification-only features.

## What actually compiles today?

Structs and pointers, the full operator set (including compound assignment and the `&.` `|.` `^.` bitwise family), `when` / `match` pattern matching, `for` loops including iterators, `state`/`dock`/`jump`, generic instantiation and bounds, function overloading, traits/interfaces and `dyn` method dispatch, macros and `raw macro`, `raw opaque`, `lend`/`view`, modules, and C header imports. Tag macros, comptime, `const fn`, async execution, contexts/words, and the full NRA ownership analysis are not implemented in `Zith--`.

## Is this a Rust replacement?

No. Zith is an independent language experiment. Compare concrete compiler behavior and ecosystem maturity rather than assuming equivalent safety or tooling guarantees.

## Where are game, ECS, and scene docs?

They are not active language-guide sections because they are not established in the current specification and implementation status. This portal documents the language and compiler state that can be sourced canonically.
