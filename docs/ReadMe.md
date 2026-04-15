# Zith Language

**Status:** Work in Progress. Zith is currently under active development. The project is not yet stable or ready for production use, but community feedback and contributions are welcome.

## Documentation

**[Read the Complete Documentation](https://galaxyhaze.github.io/Zith/docs/index.html?page=intro)** | [View on GitHub](https://github.com/GalaxyHaze/Zith/)

The documentation includes:
*   **Getting Started:** Installation and quick start tutorials.
*   **CLI Reference:** Complete command reference (`build`, `check`, `compile`, `execute`, `run`).
*   **Language Guide:** Comprehensive documentation on syntax, types, control flow, functions, and memory management.
*   **Project Configuration:** Reference for `ZithProject.toml`.

---

## About Zith

Zith is a statically-typed systems programming language designed to provide safe, explicit control over code while maintaining high performance and modern ergonomics.

Unlike traditional backends, Zith implements a complete custom toolchain:
*   **Custom Parser:** Parses Zith source code into a rich Abstract Syntax Tree (AST).
*   **Type System:** Static type checking with type inference.
*   **Bytecode Generation:** Compiles to ZBC (Zith Bytecode) format.
*   **Multi-Execution Model:** Run via the Zith VM or compile to native binaries.

---

## Architecture Overview

```
Zith Source Code (.zith)
        ↓
    Parser & Lexer
        ↓
  Abstract Syntax Tree (AST)
        ↓
    Type Checker
        ↓
  ZBC Bytecode Generator
        ↓
  ZBC File (.zbc)
        ↙        ↘
   VM Execution  LLVM Backend
   (Current)     (Planned)
```

### Compilation Pipeline

1.  **Parsing & Import Resolution:** Source files are parsed into an AST with module/import support.
2.  **Type Checking:** Full static type analysis and inference.
3.  **Bytecode Generation:** Generate portable ZBC bytecode.
4.  **Execution:** Run bytecode via the Zith Virtual Machine (current) or LLVM Backend (planned).

---

## Key Features

### Current
*   Custom Parser for Zith syntax.
*   Module/Import System for multi-file organization.
*   ZBC Bytecode (portable, versioned format).
*   Static Type System with inference.
*   CLI Tooling (`build`, `check`, `compile`, `execute`, `run`).

### In Progress
*   Zith Virtual Machine for ZBC bytecode execution.
*   Standard Library (core functionality for math, strings, I/O, collections).

### Planned
*   LLVM Backend for native machine code compilation.
*   Language Server Protocol (LSP) implementation.
*   Interactive Debugger.
*   Package Manager.

---

## Quick Start

### Installation

**Linux / macOS**
```bash
curl -sSL https://raw.githubusercontent.com/GalaxyHaze/Zith/master/install.sh | bash
```

**Windows**
```powershell
irm https://raw.githubusercontent.com/GalaxyHaze/Zith/master/install.ps1 | iex
```

**Verify Installation**
```bash
zith --version
```

### Create Your First Project

```bash
# Create a new project
zith new hello-world
cd hello-world

# Build the project
zith build

# Run the application
zith run
```

### Project Structure

```
hello-world/
├── src/
│   └── main.zith          # Entry point
├── ZithProject.toml   # Project metadata
└── README.md
```

---

## Language Overview

### Hello World

```zith
fn main() {
    println("Hello, Zith!");
}
```

### Variables and Types

```zith
fn main() {
    let x = 42;           // immutable, type inferred
    var y = 10;           // mutable
    y = 20;

    let name: []char = "Alice";   // explicit type — string slice
    println("{}", name);
}
```

### Functions

```zith
// return type uses ':'
fn add(a: i32, b: i32): i32 {
    a + b   // implicit return — last expression without ';'
}

fn main() {
    let result = add(5, 3);
    println("{}", result);
}
```

### Control Flow

```zith
fn main() {
    let x = 15;

    // parentheses are optional
    if x > 0 {
        println("Positive");
    } else (x < 0) {       // 'else (cond)' instead of 'else if'
        println("Negative");
    } else {
        println("Zero");
    }

    // for-in loop
    for i in 0..5 {
        println("{}", i);
    }

    // while-style loop
    var n = 0;
    for n < 10 {
        n += 1;
    }
}
```

### Error Handling

```zith
fn divide(a: i32, b: i32): i32! {   // '!' means may fail
    if b == 0
        throw DivisionByZero;
    a / b
}

fn main() {
    // try acquires results, catch handles errors
    try divide(10, 2) | result | {
        println("Result: {}", result);
    }
    catch {
        DivisionByZero -> println("Cannot divide by zero"),
        Error          -> println("Unexpected error")
    }

    // propagate error to caller
    let val = divide(10, 2)!;

    // fatal — terminate if error
    must! divide(10, 2) | val | {
        println("{}", val);
    }
}
```

---

## Memory Model

Zith uses a **compile-time ownership system** — memory safety is enforced without a garbage collector and without runtime overhead. Every value has a clear owner, and the compiler tracks ownership through the entire program.

There are five ownership keywords. Each has exactly one purpose.

---

### `unique` — Exclusive Ownership

A `unique` value has exactly one owner. When it is assigned to another variable, the original is invalidated — this is called a *move*.

```zith
let a = unique Resource.new();  // 'a' owns the resource
let b = a;                      // ownership moves to 'b'
a.read();                       // ❌ error — 'a' is no longer valid
b.read();                       // ✓ only 'b' is valid
```

`unique` is the default ownership model. It gives the compiler maximum information and produces zero-overhead code.

---

### `share` — Shared Ownership

A `share` value can have multiple owners simultaneously. Ownership is promoted from `unique` — this decision is **irreversible**. All shares are tracked by the compiler at compile-time, with no reference counting at runtime.

```zith
let data = Resource.new();      // unique by default

// promote to share — 'data' is now invalid
let s1 = data as share;
let s2 = s1;                    // ✓ copy — both s1 and s2 are valid
let s3 = s1;                    // ✓ another copy

s1.read();                      // ✓
s2.read();                      // ✓
s3.read();                      // ✓
```

Because there is no reference counting, `share` has zero runtime overhead. The compiler validates all accesses statically.

---

### `view` — Read-Only Reference

A `view` does not own the value — it only observes it. The compiler guarantees at compile-time that the original owner exists for the entire duration of the `view`.

```zith
fn print_info(data: view Resource) {
    // 'data' is guaranteed to exist here
    // cannot modify, cannot own
    println("{}", data.name);
}

let res = Resource.new();
print_info(view res);   // 'res' is still valid after the call
```

`view` is the standard way to pass values for reading — zero cost, zero ownership transfer.

---

### `lend` — Mutable Reference

A `lend` allows temporary mutable access without transferring ownership. The compiler guarantees that the original owner exists and that no other mutable access occurs simultaneously.

```zith
fn increment(counter: lend i32) {
    counter += 1;
}

var x = 0;
increment(lend x);   // x is temporarily lent
println("{}", x);    // x is still valid — prints 1
```

`lend` is the standard way to pass values for modification. Only one `lend` can exist at a time for a given value.

---

### `extension` — Hierarchical Ownership

`extension` is a unique Zith concept. It expresses that object B is a **structural part** of object A — B cannot outlive A, cannot free A, and can only access A temporarily. The compiler verifies the hierarchy statically.

This solves a fundamental problem: doubly-linked lists, trees with parent pointers, and component systems — all without weak pointers or runtime checks.

```zith
struct Engine {
    cylinders: []Cylinder,
    power:     u32
}

struct Cylinder {
    engine:   extension Engine,  // 'Cylinder belongs to Engine'
    position: u32
}
```

The compiler knows:
- A `Cylinder` cannot outlive its `Engine`
- A `Cylinder` cannot free its `Engine`
- Access to `engine` inside `Cylinder` is always temporary and safe

**Doubly-linked list with `extension`:**

```zith
struct Node<T> {
    value: T,
    next:  share Node<T>?,    // owns the next node
    prev:  extension Node<T>? // part of the chain — no ownership
}
```

No weak pointers. No runtime checks. No reference counting. The compiler proves safety entirely at compile-time.

**Splitting and reconnecting:**

If you need to move a child out of its parent temporarily, use `split` and `connect` from the `Extension` trait:

```zith
struct Parent {
    child: extension Child?   // '?' means the child may be absent
}

// remove child from hierarchy — child becomes unique
let detached = parent.child.split();

process(detached);   // 'detached' lives independently as unique

// reconnect to the hierarchy
detached.connect(parent);
// parent.child is valid again
```

---

### Ownership Summary

| Keyword     | Owns | Exclusive | Compile-time | Mutable | Notes                          |
|-------------|------|-----------|--------------|---------|--------------------------------|
| `unique`    | yes  | yes       | yes          | yes     | default, move semantics        |
| `share`     | yes  | no        | yes          | yes     | multiple owners, no ref count  |
| `view`      | no   | no        | yes          | no      | read-only reference            |
| `lend`      | no   | no        | yes          | yes     | temporary mutable access       |
| `extension` | no   | no        | yes          | yes     | hierarchical part-of relation  |

All five keywords are verified entirely at compile-time. There is no garbage collector, no reference counting, and no runtime cost for ownership tracking.

---

## Unique Language Concepts

### Function Kinds

Zith distinguishes four kinds of functions at the type level. Each kind has explicit rules enforced by the compiler.

```zith
fn normal() { ... }           // standard function
async fn coroutine() { ... }  // may yield — cooperative multitasking
noreturn fn machine() { ... } // never returns — uses markers and goto
flowing fn controlled() { ... }// may use goto but has a return
```

This is not just documentation — the compiler rejects `yield` outside `async fn`, `goto` outside `noreturn`/`flowing fn`, and `return` inside `noreturn fn`.

---

### Markers and Structured Goto

`noreturn fn` and `flowing fn` use **markers** as named jump targets with bodies and parameters. This is not the dangerous `goto` of C — markers are structured, scoped, and verified by the compiler.

```zith
noreturn fn game_loop(state: lend GameState) {
    marker update(dt: f32) {
        state.physics.step(dt);
        goto render;
    }

    marker render() {
        state.draw();
        goto update(0.016);
    }

    entry { goto update(0.0); }
}
```

This replaces ad-hoc state machines, manual loop unwinding, and unsafe goto patterns with a safe, readable, and verifiable construct. The compiler tracks all jump targets and verifies that every path is valid.

---

### Named Operators

Zith allows user-defined operators with names instead of symbols. Named operators must be explicitly activated with `use infix` before use — the parser cannot recognise them otherwise, and this makes every active operator visible at the top of the file.

```zith
import std.math.vector as vector;

use infix = vector { dot, cross };  // activate named operators

fn main() {
    let v1 = vector.Vec3 { x: 1.0, y: 0.0, z: 0.0 };
    let v2 = vector.Vec3 { x: 0.0, y: 1.0, z: 0.0 };

    let d = v1 dot v2;    // named infix operator
    let c = v1 cross v2;  // named infix operator
    println("dot: {}", d);
}
```

This avoids the classic problem of operator overloading — `*` meaning multiplication, dot product, or pointer dereference depending on context. Named operators are always unambiguous.

---

### Contexts and DSLs

A `context` defines a self-contained environment with its own operators, constants, and macros. Activating a context with `use context` injects that environment into a block without polluting the surrounding scope.

```zith
context Math {
    const PI          = 3.141592653589793;
    const GoldenRatio = 1.618033988749895;

    operator dot(a: Vec2, b: Vec2): infix {
        (a.x * b.x) + (a.y * b.y)
    }
}

fn area(r: f32): f32 {
    use context Math {
        PI * r * r   // PI and dot available here
    }
    // PI and dot do not exist outside the block
}
```

This enables embedded DSLs that are scoped, composable, and leave no trace outside their activation block.

---

### do / error / drop

Every scope can define three lifecycle blocks. Together they provide structured resource management with full error context — without exceptions, without hidden destructors.

```zith
do (let file = open("data.txt")!, let db = connect()!) {
    // main work — file and db available here
    process(file, db)!;
}
error(Error e) {
    // intercept — does NOT handle, only enriches before propagating
    throw LinkedError {
        current:  ProcessError.Failed,
        previous: e
    };
}
drop {
    // always executes — regardless of success, error, or return
    file.close();
    db.disconnect();
}
```

- `do` — acquires resources and performs work
- `error` — intercepts errors to add context, then rethrows
- `drop` — guaranteed cleanup, always runs

This pattern is especially powerful for scenes, transactions, and any resource that requires paired setup and teardown.

---

### The Anchor Pattern

For data structures with cyclic references (graphs, doubly-linked lists), Zith recommends the **Anchor pattern** — a centralised owner that holds all nodes, with navigation done via opaque generational IDs instead of pointers.

```zith
struct Anchor<T> {
    slots:     []Slot<T>,
    allocator: Allocator,
    head:      NodeId<T>?
}

// NodeId is opaque — cannot be fabricated outside Anchor
struct NodeId<T> {
    private:
        index:      u32,
        generation: u32
}
```

Because `NodeId` is opaque and generational, using a stale ID (one whose slot was freed and reused) safely returns `null` instead of accessing wrong data. There are no dangling pointers, no ownership cycles, and no runtime cost beyond a single generation check.

```zith
let anchor = Anchor<City>.new(allocator);
let lisbon = anchor.insert(City { name: "Lisbon" });
let porto  = anchor.insert(City { name: "Porto"  });

// navigate by ID — no ownership concerns
let city = anchor.get(lisbon);  // view City? — null if stale
```

---

## CLI Commands

### Build

```bash
# Debug build
zith build

# Release build
zith build -m release
```

### Check

```bash
# Syntax and type checking without compilation
zith check
```

### Compile

```bash
# Compile to ZBC bytecode
zith compile src/main.zith -o main.zbc

# Compile to assembly (planned)
zith compile src/main.zith --emit asm -o main.s
```

### Execute

```bash
# Run a compiled ZBC file using the VM
zith execute bin/app.zbc

# Build and run in one command
zith run
```

---

## Project Status

### Completed
- [x] Lexer & Parser
- [x] AST Construction
- [x] Type System
- [x] Module System
- [x] ZBC Generation
- [x] CLI Tools
- [x] Documentation Site

### In Progress
- [ ] Zith Virtual Machine
- [ ] Standard Library
- [ ] Performance Optimization

### Planned
- [ ] LLVM Backend
- [ ] Language Server Protocol (LSP)
- [ ] Interactive Debugger
- [ ] Package Manager
- [ ] FFI Support

---

## Building from Source

### Prerequisites

*   Git
*   CMake (3.15+)
*   C++ Compiler (GCC, Clang, or MSVC with C++17 support)

### Build Steps

```bash
git clone https://github.com/GalaxyHaze/Zith.git
cd Zith
mkdir build && cd build
cmake ..
make
```

---

## Design Principles

1.  **Explicit over implicit:** Every behaviour is visible in the code. Hidden costs, hidden ownership, and hidden control flow are avoided.
2.  **Each keyword does one thing:** No keyword is overloaded with multiple unrelated meanings.
3.  **Safe by default:** Memory and type safety are enforced at compile-time without runtime overhead.
4.  **Zero-cost abstractions:** High-level features compile to efficient machine code without hidden overhead.
5.  **Complexity appears when needed:** Simple code stays simple. Advanced features are available but never forced.

---

## Contributing

Zith welcomes community contributions.

### Reporting Issues
Please open an issue with a clear description, steps to reproduce, and environment details.

### Contributing Code
1.  Fork the repository.
2.  Create a feature branch.
3.  Implement changes and add tests.
4.  Submit a Pull Request with a clear commit message.

*Note: The API, syntax, and compiler behaviour are subject to change during the development phase.*

---

## Resources

*   [Documentation](https://galaxyhaze.github.io/Zith/docs/index.html?page=intro)
*   [Issue Tracker](https://github.com/GalaxyHaze/Zith/issues)
*   [Discussions](https://github.com/GalaxyHaze/Zith/discussions)
*   [GitHub Repository](https://github.com/GalaxyHaze/Zith)

---

## FAQ

**Q: When will Zith be production-ready?**  
A: The project is currently under active development. Focus is on the VM and standard library.

**Q: What is the difference between ZBC and native compilation?**  
A: ZBC bytecode runs in the Zith VM for portability. Native compilation via LLVM produces machine code for maximum performance.

**Q: How does Zith achieve memory safety without a garbage collector?**  
A: Through compile-time ownership tracking. The five ownership keywords (`unique`, `share`, `view`, `lend`, `extension`) give the compiler enough information to verify all memory accesses statically. No runtime checks, no reference counting.

**Q: Is Zith suitable for kernel and embedded development?**  
A: Yes. The `raw` keyword provides an explicit escape hatch for hardware-level programming, direct memory access, and FFI with C. The ownership system and `raw` are deliberately separate — safe code and unsafe code are always visually distinct.

---

## License

Zith is licensed under the [MIT License](./license).
