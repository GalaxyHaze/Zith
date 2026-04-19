---
id: rust-comparison
title: Zith vs Rust
sidebar_label: Rust Comparison
description: Detailed comparison between Zith and Rust - when to choose each language.
---

# Zith vs Rust

Both Zith and Rust offer memory safety without garbage collection, but they take different approaches.

## Quick Comparison

| Feature | Rust | Zith | Winner |
|---------|------|------|--------|
| **Memory Safety** | ✅ Borrow checker | ✅ Ownership types | Tie |
| **Learning Curve** | ⚠️ Very steep | ✅ Gentle | 🏆 Zith |
| **Syntax** | Complex keywords | C-like | 🏆 Zith |
| **Lifetimes** | Explicit annotations | Implicit | 🏆 Zith |
| **Compile Time** | Slow | Fast | 🏆 Zith |
| **Ecosystem** | Large, mature | Growing | 🏆 Rust |
| **Safety Guarantees** | Maximum | Strong | 🏆 Rust |
| **Beginner Friendly** | ❌ | ✅ | 🏆 Zith |
| **Native ECS** | ❌ (external crates) | ✅ Built-in | 🏆 Zith |
| **Safe DSLs** | ⚠️ (macros) | ✅ Contexts | 🏆 Zith |

## Key Differences

### 1. Ownership Model

**Rust:** Uses a borrow checker with lifetimes
```rust
fn process<'a>(data: &'a mut Vec<i32>) -> &'a i32 {
    &data[0]  // Lifetime annotations required
}
```

**Zith:** Ownership visible in types, no lifetime annotations
```zith
fn process(data: mut [i32]): view i32 {
    return data[0];  // No lifetimes needed
}
```

### 2. Error Messages

**Rust:** Can be complex, especially with lifetimes
```
error[E0495]: cannot infer an appropriate lifetime due to conflicting requirements
  --> src/main.rs:10:5
   |
10 |     fn get(&self) -> &i32 { &self.data }
   |     ^^^^^^^^^^^^^^^^^^^^^
   |
note: first, the lifetime cannot outlive the anonymous lifetime #1 defined on line 9...
  --> src/main.rs:9:12
   |
9  | struct Foo { data: i32 }
   |            ^^^^
note: ...but the lifetime must also include the declared lifetime parameter 'a
  --> src/main.rs:8:16
   |
8  | impl<'a> Foo {
   |                ^
```

**Zith:** Simpler, more direct
```
error[E005]: Invalid ownership modifier
  --> src/main.zith:10:8
   |
10 | let ref: mut i32 = data;  // data is already borrowed
   |         ^^^ Cannot create mutable reference while other references exist
   |
   = help: Use 'view' for read-only access or wait for existing references to end
```

### 3. Syntax Complexity

**Rust:** Many special keywords and concepts
```rust
let x: Box<dyn Iterator<Item = i32> + 'static> = Box::new(vec![1, 2, 3].into_iter());
```

**Zith:** More straightforward
```zith
let x: Iterator<i32> = vec![1, 2, 3].iter();
```

### 4. Generics

**Rust:** Powerful but verbose
```rust
fn process<T: Clone + Debug>(items: Vec<T>) -> Vec<T> 
where 
    T: PartialEq,
{
    items.clone()
}
```

**Zith:** Cleaner syntax
```zith
fn process<T>(items: [T]) -> [T] 
where T: Clone + Debug + PartialEq 
{
    return items.clone();
}
```

## When to Choose Zith

### ✅ Choose Zith If:

1. **You're learning systems programming**
   - Gentler learning curve
   - Faster feedback from compiler
   - Less frustration with borrow checker

2. **You're building games**
   - Native ECS support
   - Scene management built-in
   - Performance-focused design

3. **You want rapid prototyping**
   - Faster compilation
   - Simpler syntax
   - Less boilerplate

4. **You need safe DSLs**
   - Context system for embedded languages
   - No macro complexity
   - Type-safe by design

5. **Your team has mixed experience levels**
   - Easier onboarding for juniors
   - C-like familiarity for seniors
   - Consistent code style

## When to Choose Rust

### ✅ Choose Rust If:

1. **Maximum safety is critical**
   - More mature safety guarantees
   - Larger body of proven code
   - More security audits

2. **You need a large ecosystem**
   - Crates.io has 100k+ packages
   - More libraries for common tasks
   - Better community support

3. **Production deployment is imminent**
   - More battle-tested
   - Better tooling maturity
   - Larger talent pool

4. **You need specific platforms**
   - WASM support is excellent
   - More OS/architecture support
   - Better mobile support

5. **Formal verification matters**
   - More research backing
   - Formal methods integration
   - Academic interest

## Migration Considerations

### From Rust to Zith

**Easier:**
- Ownership concepts transfer directly
- Similar performance characteristics
- Zero-cost abstractions

**Challenges:**
- Smaller ecosystem
- Different error handling
- Learning Zith-specific features (contexts, scenes)

### From C/C++ to Zith

**Easier:**
- Familiar C-like syntax
- No radical paradigm shift
- Gradual feature adoption

**Benefits:**
- Memory safety without GC
- Modern tooling
- Better error messages

## Code Comparison Example

### Same Program in Both Languages

**Rust:**
```rust
use std::collections::HashMap;

fn main() {
    let mut scores: HashMap<&str, i32> = HashMap::new();
    scores.insert("Alice", 95);
    scores.insert("Bob", 87);
    
    for (name, score) in &scores {
        println!("{}: {}", name, score);
    }
    
    if let Some(alice_score) = scores.get("Alice") {
        println!("Alice scored: {}", alice_score);
    }
}
```

**Zith:**
```zith
fn main() {
    let mut scores = Map<str, i32>.new();
    scores["Alice"] = 95;
    scores["Bob"] = 87;
    
    for (name, score) in scores {
        print("{name}: {score}");
    }
    
    if let alice_score = scores.get("Alice") {
        print("Alice scored: {alice_score}");
    }
}
```

## The Bottom Line

**Zith** is for developers who want:
- Memory safety without complexity
- Fast iteration and compilation
- Built-in features for specific domains (games, DSLs)
- A gentler introduction to systems programming

**Rust** is for developers who need:
- Maximum safety guarantees
- A mature ecosystem
- Production-proven track record
- Platform diversity

Both are excellent choices. The best depends on your specific needs, team expertise, and project requirements.

---

**Next:** Explore [Use Cases](./05-use-cases.md) →
