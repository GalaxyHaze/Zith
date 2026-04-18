---
id: quickstart
title: Quick Start
sidebar_label: Quick Start
description: Write your first Zith program in minutes. A beginner-friendly introduction.
---

# Quick Start

Let's write your first Zith program! This guide will get you up and running in minutes.

## Step 1: Create a New Project

```bash
# Create a new project
zith new hello_world
cd hello_world

# Project structure created:
# hello_world/
# ├── ZithProject.toml
# └── src/
#     └── main.zith
```

## Step 2: Write Your First Program

Open `src/main.zith` and replace the content with:

```zith
fn main() {
    print("Hello, Zith!");
    
    // Variables are declared with 'let'
    let name: str = "Developer";
    let age: u32 = 25;
    
    // String interpolation
    print("Welcome, {name}! You are {age} years old.");
    
    // Basic arithmetic
    let x: i32 = 10;
    let y: i32 = 5;
    
    print("{x} + {y} = {x + y}");
    print("{x} - {y} = {x - y}");
    print("{x} * {y} = {x * y}");
    print("{x} / {y} = {x / y}");
}
```

## Step 3: Run Your Program

```bash
# Run directly
zith run src/main.zith

# Or build and run
zith build
./build/hello_world
```

**Expected output:**
```
Hello, Zith!
Welcome, Developer! You are 25 years old.
10 + 5 = 15
10 - 5 = 5
10 * 5 = 50
10 / 5 = 2
```

## Understanding the Basics

### Functions

Functions in Zith look similar to C but with type annotations after the parameter name:

```zith
// Function with parameters and return type
fn add(a: i32, b: i32): i32 {
    return a + b;
}

// Function without return value (void)
fn greet(name: str) {
    print("Hello, {name}!");
}
```

### Variables and Types

Zith is statically typed with type inference:

```zith
// Explicit type annotation
let count: u32 = 42;
let pi: f64 = 3.14159;
let message: str = "Hello";

// Type inference (compiler figures it out)
let inferred = 100;  // i32 by default
let text = "World";  // str
```

### Control Flow

```zith
// If-else statements
let score: i32 = 85;

if score >= 90 {
    print("Excellent!");
} else if score >= 70 {
    print("Good job!");
} else {
    print("Keep practicing!");
}

// For loops
for i in 0..10 {
    print("Count: {i}");
}

// While loops
let mut counter: i32 = 0;
while counter < 5 {
    print("Counter: {counter}");
    counter += 1;
}
```

## Try These Exercises

### Exercise 1: Calculator

Create a simple calculator:

```zith
fn main() {
    let a: i32 = 20;
    let b: i32 = 4;
    
    print("Sum: {a + b}");
    print("Difference: {a - b}");
    print("Product: {a * b}");
    print("Quotient: {a / b}");
    print("Remainder: {a % b}");
}
```

### Exercise 2: Temperature Converter

Convert Celsius to Fahrenheit:

```zith
fn celsius_to_fahrenheit(c: f64): f64 {
    return (c * 9.0 / 5.0) + 32.0;
}

fn main() {
    let temp_c: f64 = 25.0;
    let temp_f = celsius_to_fahrenheit(temp_c);
    
    print("{temp_c}°C = {temp_f}°F");
}
```

### Exercise 3: Factorial

Calculate factorial using a loop:

```zith
fn factorial(n: u32): u64 {
    let mut result: u64 = 1;
    
    for i in 1..(n + 1) {
        result *= i as u64;
    }
    
    return result;
}

fn main() {
    let num: u32 = 5;
    print("{num}! = {factorial(num)}");
}
```

## Common Commands

| Command | Description |
|---------|-------------|
| `zith new <name>` | Create new project |
| `zith run <file>` | Run a Zith file directly |
| `zith build` | Build project |
| `zith check` | Check for errors without building |
| `zith fmt` | Format code |
| `zith clean` | Remove build artifacts |

## What's Next?

You've written your first Zith programs! Continue learning:

1. **[Why Zith?](./why-zith.md)** - Understand Zith's advantages
2. **[Syntax Basics](../technical/language/syntax.md)** - Deep dive into syntax
3. **[CLI Reference](../technical/cli/overview.md)** - Master the command-line tools
4. **[Language Guide](../technical/language/overview.md)** - Comprehensive language features

---

**Need help?** Visit our [FAQ](../technical/faq/overview.md) or join the community on [GitHub](https://github.com/galaxyhaze/Zith/discussions).
