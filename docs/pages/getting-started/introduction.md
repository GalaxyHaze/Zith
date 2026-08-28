---
id: getting-started-introduction
title: Introduction
section: Getting Started
output: getting-started/D-introduction.html
aliases: intro/D-overview.html
kind: editorial
---
# Introduction

Zith is a statically typed compiled language with a specification that is ahead of its implementation. This portal is therefore language documentation in draft form, not a stability guarantee.

The `main` compiler builds `Zith--`, the implemented subset of the language. The [Zith-- reference](doc:reference-zith-subset) is the operational language for `zithc`; the [Specification](doc:reference-specification) describes the larger intended language.

## Read the right document

- Start with [Quick Start](doc:getting-started-quick-start) to create and run a project.
- Use the [Language Guide](doc:guide-overview) for short practical explanations.
- Use the [Language Reference](doc:reference-specification) for the canonical specification.
- Use the [Zith-- reference](doc:reference-zith-subset) for the implemented subset.
- Check [Implementation Status](doc:reference-implementation-status) before using a feature in production code.

The `Zith--` compiler currently supports core declarations, bindings, structs and pointers, the full operator set including compound assignment and bitwise operators, `when` pattern matching, the `for` family including iterators, `state`/`dock`/`jump`, generic instantiation, function overloading, traits/interfaces and `dyn` method dispatch, macros and `raw macro`, `raw opaque` handles, `lend`/`view`, modules, and C header imports. All of it lowers through HIR to LLVM code generation for x86-64 and WebAssembly. Tag macros parse, but are rejected in the `Zith--` pipeline.

Ownership analysis (NRA) has the `lend`/`view` call-annotation slice but not the full state machine. Comptime evaluation, `const fn`, async execution, and context-based extensions remain specification-only; `const fn` and `global` report `E2010`.
