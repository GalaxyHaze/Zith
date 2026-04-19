---
id: philosophy
title: Language Philosophy
sidebar_label: Philosophy
description: Understanding the design principles and philosophy behind Zith.
---

# Language Philosophy

Zith was created with clear principles that guide every design decision.

## Why Zith Exists

Zith was born from a simple observation: **systems programming languages had become either too complex or too unsafe**.

### The Gap We Fill

| Language | Safety | Complexity | Verdict |
|----------|--------|------------|---------|
| C | ❌ | ✅ Low | Unsafe but simple |
| C++ | ⚠️ | ❌ Extreme | Complex, legacy baggage |
| Rust | ✅ | ❌ High | Safe but steep learning curve |
| Zig | ✅ | ✅ Moderate | Good balance, limited features |
| **Zith** | ✅ | ✅ Low-Moderate | **Safe AND accessible** |

## Core Design Principles

### 1. Clarity Over Cleverness

Every language feature should be immediately understandable:

```zith
// Clear ownership - no hidden magic
fn process(mut self: Health, dmg: view u16) {
    // 'mut self' = I own this and can modify
    // 'view u16' = read-only reference
}
```

### 2. Progressive Complexity

Start simple, add features only when needed:

- **Level 1**: C-like syntax for beginners
- **Level 2**: Components and entities for game dev
- **Level 3**: Contexts for DSLs and advanced metaprogramming

### 3. Zero Runtime Overhead

Everything happens at compile-time:

- No garbage collector
- No hidden allocations
- No runtime type information (unless explicitly requested)

### 4. Explicit Is Better Than Implicit

Ownership, mutability, and effects are visible in types:

```zith
// You can see ownership at a glance
let data: unique [i32] = alloc.new([1, 2, 3]);
let ref: view [i32] = data;      // Read-only borrow
let mut_ref: mut [i32] = data;   // Mutable borrow (requires transfer)
```

### 5. Safety Without Punishment

The compiler helps you, not fights you:

- Clear error messages with suggestions
- Gradual learning curve for ownership
- Escape hatches when needed (`unsafe` blocks)

## Trade-offs We Made

### ✅ What We Gained

- **Accessibility**: Easier to learn than Rust
- **Expressiveness**: Rich feature set for complex domains
- **Performance**: Zero-cost abstractions
- **Safety**: Compile-time guarantees without GC

### ⚠️ What We Accepted

- **Smaller ecosystem**: Growing, but not as large as Rust/C++
- **Less proven**: Newer language, fewer battle-tested projects
- **Tooling maturity**: Improving rapidly but still developing

## Our Vision

Zith aims to be:

1. **The best language for learning systems programming**
2. **A practical choice for game engines and graphics**
3. **A bridge between C simplicity and modern safety**
4. **A platform for safe DSL creation**

We're not trying to replace Rust for all use cases. Instead, we offer an alternative that prioritizes **accessibility without sacrificing safety**.

## Guiding Questions

Every feature proposal is evaluated against:

1. Does this make common tasks easier?
2. Does this introduce hidden complexity?
3. Can beginners understand this within their first week?
4. Does this maintain zero runtime overhead?
5. Does this improve safety without hurting ergonomics?

If a feature doesn't pass these tests, it doesn't make it into Zith.

---

**Next:** Learn about [Security](./03-security.md) →
