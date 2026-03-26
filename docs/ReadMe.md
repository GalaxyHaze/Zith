# Kalidous Language

**Status:** Work in Progress. Kalidous is currently under active development. The project is not yet stable or ready for production use, but community feedback and contributions are welcome.

## Documentation

**[Read the Complete Documentation](https://galaxyhaze.github.io/Kalidous-Lang/docs/index.html?page=intro)** | [View on GitHub](https://github.com/GalaxyHaze/Kalidous-Lang/)

The documentation includes:
*   **Getting Started:** Installation and quick start tutorials.
*   **CLI Reference:** Complete command reference (`build`, `check`, `compile`, `execute`, `run`).
*   **Language Guide:** Comprehensive documentation on syntax, types, control flow, functions, and memory management.
*   **Project Configuration:** Reference for `KalidousProject.toml`.

## About Kalidous

Kalidous is a statically-typed systems programming language designed to provide safe, explicit control over code while maintaining high performance and modern ergonomics.

Unlike traditional backends, Kalidous implements a complete custom toolchain:
*   **Custom Parser:** Parses Kalidous source code into a rich Abstract Syntax Tree (AST).
*   **Type System:** Static type checking with type inference.
*   **Bytecode Generation:** Compiles to KBC (Kalidous Bytecode) format.
*   **Multi-Execution Model:** Run via the Kalidous VM or compile to native binaries.

## Architecture Overview

```
Kalidous Source Code (.kali)
        ↓
    Parser & Lexer
        ↓
  Abstract Syntax Tree (AST)
        ↓
    Type Checker
        ↓
  KBC Bytecode Generator
        ↓
  KBC File (.kbc)
        ↙        ↘
   VM Execution  LLVM Backend
   (Current)     (Planned)
```

### Compilation Pipeline

1.  **Parsing & Import Resolution:** Source files are parsed into an AST with module/import support.
2.  **Type Checking:** Full static type analysis and inference.
3.  **Bytecode Generation:** Generate portable KBC bytecode.
4.  **Execution:** Run bytecode via the Kalidous Virtual Machine (current) or LLVM Backend (planned).

## Key Features

### Current
*   Custom Parser for Kalidous syntax.
*   Module/Import System for multi-file organization.
*   KBC Bytecode (portable, versioned format).
*   Static Type System with inference.
*   CLI Tooling (`build`, `check`, `compile`, `execute`, `run`).

### In Progress
*   Kalidous Virtual Machine for KBC bytecode execution.
*   Standard Library (core functionality for math, strings, I/O, collections).

### Planned
*   LLVM Backend for native machine code compilation.
*   Language Server Protocol (LSP) implementation.
*   Interactive Debugger.
*   Package Manager.

## Quick Start

### Installation

**Linux / macOS**
```bash
curl -sSL https://raw.githubusercontent.com/GalaxyHaze/Kalidous-Lang/main/install.sh | bash
```

**Windows**
```powershell
irm https://raw.githubusercontent.com/GalaxyHaze/Kalidous-Lang/main/install.ps1 | iex
```

**Verify Installation**
```bash
kalidous --version
```

### Create Your First Project

```bash
# Create a new project
kalidous new hello-world
cd hello-world

# Build the project
kalidous build

# Run the application
kalidous run
```

### Project Structure

```
hello-world/
├── src/
│   └── main.kali          # Entry point
├── KalidousProject.toml   # Project metadata
└── README.md
```

## Language Overview

### Hello World

```kalidous
fn main() {
  println("Hello, Kalidous!");
}
```

### Variables and Types

```kalidous
fn main() {
  let x = 42;              // Immutable, type inferred
  let mut y = 10;          // Mutable
  y = 20;

  let name: str = "Alice"; // Explicit type
  println(name);
}
```

### Functions

```kalidous
fn add(a: i32, b: i32) -> i32 {
  a + b  // Return value
}

fn main() {
  let result = add(5, 3);
  println(result);
}
```

### Control Flow

```kalidous
fn main() {
  let x = 15;

  if x > 0 {
    println("Positive");
  } else if x < 0 {
    println("Negative");
  } else {
    println("Zero");
  }

  for i in 0..5 {
    println(i);
  }
}
```

### Error Handling

```kalidous
fn divide(a: i32, b: i32) -> Result<i32, str> {
  if b == 0 {
    Err("Division by zero")
  } else {
    Ok(a / b)
  }
}

fn main() {
  match divide(10, 2) {
    Ok(result) => println("Result:", result),
    Err(err) => println("Error:", err),
  }
}
```

## CLI Commands

### Build

```bash
# Debug build
kalidous build

# Release build
kalidous build -m release
```

### Check

```bash
# Syntax and type checking without compilation
kalidous check
```

### Compile

```bash
# Compile to machine code
kalidous compile src/main.kali -o main.o

# Compile to assembly
kalidous compile src/main.kali --emit asm -o main.s
```

### Execute

```bash
# Run using the VM
kalidous execute bin/app

# Build and run in one command
kalidous run
```

## Project Status

### Completed
- [x] Lexer & Parser
- [x] AST Construction
- [x] Type System
- [x] Module System
- [x] KBC Generation
- [x] CLI Tools
- [x] Documentation Site

### In Progress
- [ ] Kalidous Virtual Machine
- [ ] Standard Library
- [ ] Performance Optimization

### Planned
- [ ] LLVM Backend
- [ ] Language Server Protocol (LSP)
- [ ] Interactive Debugger
- [ ] Package Manager
- [ ] FFI Support

## Building from Source

### Prerequisites

*   Git
*   CMake (3.15+)
*   C++ Compiler (GCC, Clang, or MSVC with C++17 support)

### Build Steps

```bash
git clone https://github.com/GalaxyHaze/Kalidous-Lang.git
cd Kalidous-Lang
mkdir build && cd build
cmake ..
make
```

## Design Principles

1.  **Explicit & Clear:** Code behavior should be clear. Implicit behavior is minimized.
2.  **Safe by Default:** Memory and type safety are enforced at compile time.
3.  **Zero-Cost Abstractions:** High-level features compile to efficient machine code without hidden overhead.
4.  **Modern Syntax:** Inspired by contemporary language design best practices.
5.  **Portable:** KBC bytecode runs on any platform supported by the Kalidous VM.

## Contributing

Kalidous welcomes community contributions.

### Reporting Issues
Please open an issue with a clear description, steps to reproduce, and environment details.

### Contributing Code
1.  Fork the repository.
2.  Create a feature branch.
3.  Implement changes and add tests.
4.  Submit a Pull Request with a clear commit message.

*Note: The API, syntax, and compiler behavior are subject to change during the development phase.*

## Resources

*   [Documentation](https://galaxyhaze.github.io/Kalidous-Lang/docs/index.html?page=intro)
*   [Issue Tracker](https://github.com/GalaxyHaze/Kalidous-Lang/issues)
*   [Discussions](https://github.com/GalaxyHaze/Kalidous-Lang/discussions)
*   [GitHub Repository](https://github.com/GalaxyHaze/Kalidous-Lang)

## FAQ

**Q: When will Kalidous be production-ready?**
A: The project is currently under active development. Focus is on the VM and standard library.

**Q: What is the difference between KBC and native compilation?**
A: KBC bytecode runs in the Kalidous VM for portability. Native compilation via LLVM produces machine code for maximum performance.

## License

Kalidous is licensed under the [MIT License](./license).
