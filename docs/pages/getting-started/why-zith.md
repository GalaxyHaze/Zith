---
id: getting-started-why-zith
title: Why Zith
section: Getting Started
output: getting-started/D-why-zith.html
aliases: intro/D-why-zith.html
kind: editorial
---
# Why Zith

Zith explores a systems-language design that combines a small everyday core
with explicit tools for domain-specific work. The sibling language Zith-- is
implemented by `zithc`, which supports typed functions, algebraic data types,
modules, traits, C interop, generic instantiation, and LLVM or WebAssembly
lowering. These compiler capabilities do not establish an implementation
status for Zith.

## Why the distinction matters

Zith and Zith-- have separate language contracts. The [Zith-- documentation](doc:zith-subset-overview)
covers the language compiled by `zithc`; the [Zith reference](doc:zith-overview)
covers the Zith language design. The [Implementation Status](doc:reference-implementation-status)
page records compiler support for Zith-- and does not define a roadmap between
the languages.

## A practical starting point

Use Zith-- when you want to write programs with the current compiler:

- explicit types and predictable conversions;
- low-level access through pointers, `raw opaque`, and C interop;
- generic data structures without a separate runtime type system;
- traits and interfaces for shared behaviour;
- a compiler pipeline that can target native code or WebAssembly.

Start with the [First Program](doc:getting-started-quick-start) and the
[Zith-- Language Guide](doc:guide-overview). Read the Zith reference to study
Zith's design; its features do not automatically apply to Zith--.
