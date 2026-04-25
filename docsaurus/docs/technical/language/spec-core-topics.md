---
id: spec-core-topics
title: Zith Spec Core Topics
sidebar_label: Spec Core Topics
description: A concise map of the Zith v2.0 specification topics and how they fit together.
---

# Zith Spec Core Topics

This document maps the prototype specification in `Zith-spec.md` into documentation-friendly sections.

## 1) Introduction & Philosophy

Zith targets a universal systems workflow: explicit behavior, compile-time safety, and zero-cost abstractions.

- **Explicit over implicit** semantics.
- **Safety by default** without a garbage collector.
- **Universal applicability** through contexts and data-oriented patterns.

## 2) Node Resource Model (NRM)

NRM models values as nodes and bindings as edges in a graph analyzed at compile time.

- Tracks connectivity, validity, and move state.
- Distinguishes origins: stack, heap, and custom allocators.
- Performs branch-aware liveness checks for failable/optional outcomes.

## 3) Ownership Keywords (Five-Rule Model)

Zith defines five ownership/borrowing modes:

- `unique` — exclusive ownership and moves.
- `share` — shared ownership.
- `view` — read-only borrow.
- `lend` — temporary mutable borrow.
- `extension` — parent-child structural relationship.

## 4) Type System & Type Navigation

The specification combines nominal and structural typing, and introduces safe navigation in failable/optional paths.

- Primitives, user types, generics, and trait-oriented capabilities.
- Optional/failable navigation patterns designed for explicit error-aware code.

## 5) Lexical Structure, Operators, and Contexts

The spec formalizes syntax/operator behavior and introduces **contexts** as scoped DSL namespaces.

- Contexts allow domain-specific operators and naming rules.
- Useful for SQL-like, graphics, or protocol-oriented embedded DSLs.

## 6) Packs, Flows, and Functions

- **Packs** support grouped values with ownership-aware behavior.
- **Flow constructs** make control transfer explicit.
- Functions integrate ownership rules directly in signatures.

## 7) Data-Oriented Architecture (ECS & Scenes)

The spec includes first-class patterns for entity/component systems and scene/resource boundaries.

- ECS for cache-friendly simulation logic.
- Scenes for lifecycle-scoped allocation and cleanup.

## 8) Intrinsics, Error Handling, and Concurrency

- `@` intrinsics provide compiler/runtime-level functionality.
- Try/catch and failable returns model recoverable failures.
- Concurrency and globals are constrained through ownership and trait rules.

## 9) Standard Library & Comparisons

The specification outlines standard-library direction and compares Zith's approach to contemporary systems languages.

## 10) Recommended Reading Order

1. Language Overview
2. Ownership and Memory
3. Type System and Errors
4. Contexts and DSLs
5. ECS/Scenes and architecture-level patterns

---

If you're new to Zith, start at [Language Overview](./01-overview.md), then continue with [Variables](./variables.md), [Memory](./memory.md), and [Errors](./errors.md).
