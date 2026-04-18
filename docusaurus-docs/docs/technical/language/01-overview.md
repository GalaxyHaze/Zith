---
id: overview
title: Language Guide Overview
sidebar_label: Overview
description: Comprehensive guide to Zith language features, syntax, and semantics.
---

# Language Guide

This section provides a comprehensive reference for the Zith programming language.

## Core Concepts

### [Syntax Basics](./syntax.md)
Learn the fundamental syntax of Zith including variables, types, and expressions.

### [Variables & Ownership](./variables.md)
Understand Zith's unique ownership model with `unique`, `view`, and `mut` modifiers.

### [Type System](./types.md)
Explore Zith's static type system, primitive types, and type inference.

### [Control Flow](./control-flow.md)
Master conditionals, loops, and pattern matching in Zith.

### [Functions](./functions.md)
Learn about function declaration, parameters, return types, and advanced features.

### [Memory Management](./memory.md)
Deep dive into Zith's memory model, allocation, and safe memory handling.

### [Error Handling](./errors.md)
Handle errors gracefully with Zith's error handling mechanisms.

### [Modules & Organization](./modules.md)
Structure large projects with modules, imports, and visibility controls.

### [Generics](./generics.md)
Write reusable code with Zith's powerful generics system.

## Advanced Topics

For more advanced features, see the **[Raw & Unsafe](../raw/overview.md)** section:

- Traits and type classes
- Advanced generics
- Metaprogramming
- Macros
- Unsafe operations
- Raw pointers

## Quick Reference

| Topic | Description |
|-------|-------------|
| Variables | `let x: i32 = 5;` |
| Functions | `fn add(a: i32, b: i32): i32 { a + b }` |
| Ownership | `unique`, `view`, `mut` modifiers |
| Control Flow | `if`, `for`, `while`, `match` |
| Memory | Manual with safety guarantees |
| Errors | Result types and exceptions |

---

**Ready to learn?** Start with **[Syntax Basics](./syntax.md)** →
