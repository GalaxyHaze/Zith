# Design Philosophy

## The Vision

> **"What if C++ had been designed thoughtfully from the start?"**

Zith addresses this question. It starts simple, scales gracefully, and avoids forcing unnecessary abstractions.

---

## 5 Fundamental Principles

### 1. Expressivity

Every language construct communicates intent clearly and uniquely.

```zith
// Explicit: you know the exact contract
fn process(mut self: Health, dmg: view u16) { }
         // ^explicit ownership: view = read-only
```

**Comparison with alternatives:**
- C: No ownership semantics (is it an `int*`?)
- Rust: Verbose lifetime annotations (`'a`, `'b`)
- Zith: Simple keywords (`view`, `lend`, `unique`)

---

### 2. Safety Through Typing

The compiler handles the heavy lifting by **detecting errors** through rigorous semantics, rather than relying on runtime checks or garbage collection.

```zith
let health: unique u16 = alloc.new(100);
let ref: view u16 = health;

ref += 10;  // Compile Error: view does not allow writing
```

---

### 3. Optional Features

It starts with simple C-like syntax. Components are added when necessary.

```zith
// Basic: functions like C
fn add(a: i32, b: i32): i32 { a + b }

// Advanced: uses ECS when appropriate
component Position { x, y: i32 }
entity Player { Position, ... }
```

---

### 4. Contexts for Safe DSLs

Define domain-specific languages **without strings**, **without injection**, and **without namespace pollution**.

```zith
context SQL {
    use infix = SQL.operators;
    infix operator SELECT(cols);
    infix operator FROM(table);
}

use context SQL {
    result = SELECT * FROM users WHERE age > 18;  // Type-safe!
}
```

---

### 5. Scenes as Logical Units

Each scene is an isolated resource container. There is no state leakage between levels, pages, or subsystems.

```zith
scene MainMenu { /* isolated resources */ }
scene GameLevel { /* isolated resources */ }
scene PauseMenu { /* isolated resources */ }

// Safe transition: GameLevel does not affect MainMenu
```

---

## Comparison with Alternatives

| Aspect | C | C++ | Rust | Zig | Zith |
|--------|---|-----|------|-----|----------|
| **Initial Simplicity** | Yes | No | No | Yes | Yes |
| **Explicit Ownership** | No | Partial | Yes | Partial | Yes |
| **Simple Lifetimes** | N/A | N/A | No | Partial | Yes |
| **Safe DSLs** | No | No | Partial | Partial | Yes |
| **Native ECS** | No | No | No | No | Yes |
| **Zero Overhead** | Yes | Yes | Yes | Yes | Yes |

---

## Key Design Decisions

### Why `shared` instead of `Rc`?

Rust's `Rc` is powerful but introduces overhead. `shared` is:
- Static (no dynamic reference counters)
- Clear in intent
- Free of unnecessary complexity

### Why separate Contexts from Traits?

- **Contexts** = Isolated, type-safe DSLs
- **Traits** = Polymorphic abstractions

Keeping them separate reduces conflicts, improves clarity, and enhances tooling support.

### Why are Components POD?

Components are **pure data**. Keeping them simple promotes better design:
- No hidden state
- No trait objects
- No unnecessary complexity

For complex behavior, use `entity` or `struct`.

---

## What Zith Is Not

- A direct replacement for Rust (Rust offers stronger safety guarantees)
- A "better C++" (C++ has a massive ecosystem)
- A garbage-collected language (Zero overhead is a priority)
- Suited for pure functional programming

---

## What Zith Is

- A systems language with a deliberate design philosophy
- Suitable for beginners learning low-level programming
- Designed for game engines, runtimes, and DSLs
- A bridge between C and modern languages

---

**[⬆ Back to top](#design-philosophy)**
