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

## Read the right document

- Start with [Quick Start](doc:getting-started-quick-start) to create and run a project.
- Use the [Language Guide](doc:guide-overview) for short practical explanations.
- Use the [Language Reference](doc:reference-specification) for the canonical specification.
- Check [Implementation Status](doc:reference-implementation-status) before using a feature in production code.

The compiler currently supports core declarations, bindings, structs and pointers, the full operator set including compound assignment and bitwise operators, `when` pattern matching, all three working `for` forms, function overloading, macros and tag macros, `raw opaque` handles, memory qualifiers at the type level, modules, and C header imports. All of it lowers through HIR to LLVM code generation for x86-64 and WebAssembly.

Generic declarations parse and type-check but cannot yet be instantiated. Ownership analysis (NRA) has its pipeline boundary and the `view` write rule but not the full state machine. Comptime evaluation, `const fn`, `dyn` dispatch, async execution, and context-based extensions remain specification-only.
