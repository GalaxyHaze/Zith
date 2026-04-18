---
title: What is Zith?
sidebar_label: Overview
description: Introduction to the Zith programming language - a modern systems language with clarity and safety.
---

# Welcome to Zith

:::info Quick Start

New to Zith? Start here:

1. **[Why Zith?](./why-zith.md)** - Learn why Zith is great for systems programming
2. **[Installation](./installation.md)** - Set up Zith on your machine
3. **[Quick Start](./quickstart.md)** - Write your first program

:::

## What is Zith?

**Zith** is a statically-typed systems programming language designed for developers who want:

- **Explicit control flow** without hidden magic
- **Zero-cost abstractions** that don't compromise performance
- **Memory safety** through a simple ownership model (no garbage collection)
- **Modern tooling** with a built-in package manager and language server

Zith starts simple—like C—but scales gracefully to handle complex systems with features like components, entities, and contexts for safe DSLs.

## Key Features

### 🎯 Expressivity

Every language construct communicates intent clearly:

```zith
fn process(mut self: Health, dmg: view u16) { }
         // ^ explicit ownership: view = read-only
```

### 🛡️ Safety Through Typing

The compiler detects errors through rigorous semantics:

```zith
let health: unique u16 = alloc.new(100);
let ref: view u16 = health;

ref += 10;  // Compile Error: view does not allow writing
```

### 🧩 Optional Complexity

Start simple, add features when needed:

```zith
// Basic: functions like C
fn add(a: i32, b: i32): i32 { a + b }

// Advanced: ECS when appropriate
component Position { x, y: i32 }
entity Player { Position, ... }
```

### 🔮 Contexts for Safe DSLs

Domain-specific languages without strings or injection:

```zith
context SQL {
    use infix = SQL.operators;
    infix operator SELECT(cols);
}

use context SQL {
    result = SELECT * FROM users WHERE age > 18;
}
```

### 🎬 Scenes as Logical Units

Isolated resource containers for games and applications:

```zith
scene MainMenu { /* isolated resources */ }
scene GameLevel { /* isolated resources */ }
scene PauseMenu { /* isolated resources */ }
```

## Who Is Zith For?

✅ **Beginners** learning low-level programming  
✅ **Game developers** building engines and tools  
✅ **Systems programmers** who want clarity without sacrificing control  
✅ **DSL designers** creating domain-specific languages  

## What Zith Is Not

❌ A direct replacement for Rust (Rust offers stronger safety guarantees)  
❌ A "better C++" (C++ has a massive ecosystem)  
❌ A garbage-collected language (zero overhead is a priority)  
❌ Suited for pure functional programming  

## Next Steps

| I want to... | Go to... |
|-------------|----------|
| Understand why Zith matters | [Why Zith?](./why-zith.md) |
| Install Zith | [Installation](./installation.md) |
| Write my first program | [Quick Start](./quickstart.md) |
| See CLI commands | [CLI Reference](../technical/cli/overview.md) |
| Learn the language | [Language Guide](../technical/language/overview.md) |

---

Ready to dive in? Let's explore **[Why Zith?](./why-zith.md)** →
