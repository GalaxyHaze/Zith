# Kalidous Language

> **WIP:** Kalidous is currently under active development. The project is not yet stable or ready for production use, but your feedback and contributions are very welcome!

---

## 📚 Documentation

**📖 [Read the Complete Documentation](./index.html)** | [View on GitHub](https://github.com/GalaxyHaze/Kalidous-Lang/tree/main/docs)

The full documentation includes:
- **Getting Started** - Installation and quick start tutorials
- **CLI Reference** - Complete command reference (build, check, compile, execute, run)
- **Language Guide** - Comprehensive language documentation (syntax, types, control flow, functions, memory management, and more)
- **Project Configuration** - KalidousProject.toml reference

---

## About Kalidous

Kalidous is a statically-typed, systems programming language designed to provide developers with safe, explicit control over their code while maintaining high performance and modern ergonomics.

Unlike traditional backends, Kalidous implements a complete custom toolchain:
- **Custom Parser** - Parse Kalidous source code into a rich Abstract Syntax Tree (AST)
- **Type System** - Static type checking with type inference
- **Bytecode Generation** - Compile to KBC (Kalidous Bytecode) format
- **Multi-Execution Model** - Run via the Kalidous VM, or compile to native binaries

---

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

1. **Parsing & Import Resolution** - Source files are parsed into an AST with module/import support
2. **Type Checking** - Full static type analysis and inference
3. **Bytecode Generation** - Generate portable KBC bytecode
4. **Execution** - Run bytecode via:
   - **Kalidous Virtual Machine** (current, cross-platform execution)
   - **LLVM Backend** (planned, native binary compilation)

---

## Key Features

### Current
- ✅ **Custom Parser** - Parse Kalidous syntax into AST
- ✅ **Module/Import System** - Organize code across multiple files
- ✅ **KBC Bytecode** - Portable, versioned bytecode format
- ✅ **Type System** - Static typing with inference
- ✅ **CLI Tooling** - `build`, `check`, `compile`, `execute`, `run` commands

### In Progress
- 🚀 **Kalidous Virtual Machine** - Execute KBC bytecode with full language semantics
- 🚀 **Standard Library** - Core functionality (math, strings, I/O, collections)

### Planned
- 📋 **LLVM Backend** - Compile KBC to native machine code
- 📋 **Language Server** - IDE support (VS Code, Neovim, etc.)
- 📋 **Debugger** - Interactive debugging capabilities
- 📋 **Package Manager** - Dependency management and code sharing

---

## Quick Start

### Installation

#### Linux / macOS

```bash
curl -sSL https://raw.githubusercontent.com/GalaxyHaze/Kalidous-Lang/main/install.sh | bash
```

#### Windows

```powershell
irm https://raw.githubusercontent.com/GalaxyHaze/Kalidous-Lang/main/install.ps1 | iex
```

#### Verify Installation

```bash
kalidous --version
```

### Create Your First Project

```bash
# Create a new project
kalidous new hello-world
cd hello-world

# View the generated structure
tree  # or: ls -la

# Build the project
kalidous build

# Run it!
kalidous run
```

### Project Structure

```
hello-world/
├── src/
│   └── main.kali          # Entry point
├── KalidousProject.toml   # Project metadata
├── README.md
└── .gitignore
```

**For comprehensive guides**, see the [Documentation](./docs/index.html).

---

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
  a + b  // Return value (no semicolon)
}

fn main() {
  let result = add(5, 3);
  println(result);  // Prints: 8
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
  
  while x > 0 {
    x = x - 1;
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

### Modules & Imports

```kalidous
// math.kali
pub fn add(a: i32, b: i32) -> i32 {
  a + b
}
```

```kalidous
// main.kali
import math

fn main() {
  let sum = math.add(40, 2);
  println("Sum:", sum);
}
```

For more examples and documentation, visit the [Language Guide](./docs/index.html).

---

## CLI Commands

### Build Your Project

```bash
# Debug build (optimized for development)
kalidous build

# Release build (optimized for performance)
kalidous build -m release

# Build specific file
kalidous build src/main.kali -o bin/app
```

### Check Syntax Quickly

```bash
# Fast syntax and type checking without compilation
kalidous check

# Check specific file
kalidous check src/main.kali
```

### Compile to Different Formats

```bash
# Compile single file to machine code
kalidous compile src/main.kali -o main.o

# Compile to assembly
kalidous compile src/main.kali --emit asm -o main.s

# Generate bytecode explicitly
kalidous compile src/main.kali --emit bc -o program.kbc
```

### Execute & Run

```bash
# Run using the VM
kalidous execute bin/app

# Run with program arguments
kalidous execute bin/app -- arg1 arg2

# Build and run in one command
kalidous run

# Release build and run
kalidous run -m release
```

For detailed CLI documentation, see the [CLI Reference](./docs/index.html).

---

## Project Status

### Completed ✅
- [x] Lexer & Parser - Full Kalidous syntax parsing
- [x] AST Construction - Rich abstract syntax trees
- [x] Type System - Static type checking and inference
- [x] Module System - Import and module resolution
- [x] KBC Generation - Bytecode output format
- [x] CLI Tools - All basic commands implemented
- [x] Documentation Site - Interactive docs with full language guide

### In Progress 🚀
- [ ] Kalidous Virtual Machine - KBC bytecode execution engine
- [ ] Standard Library - Core modules and utilities
- [ ] Performance Optimization - Bytecode optimization passes

### Planned 📋
- [ ] LLVM Backend - Native binary compilation
- [ ] Language Server Protocol (LSP) - IDE integration
- [ ] Interactive Debugger - Step-through debugging
- [ ] Package Manager - Dependency resolution and management
- [ ] FFI Support - C interoperability

---

## Building from Source

### Prerequisites

- **Git** - Version control
- **CMake** - Build system (3.15+)
- **C++ Compiler** - GCC, Clang, or MSVC (C++17 support required)
- **1 GB** - Disk space for build artifacts

### Build Steps

```bash
# Clone the repository
git clone https://github.com/GalaxyHaze/Kalidous-Lang.git
cd Kalidous-Lang

# Create build directory
mkdir build && cd build

# Configure
cmake ..

# Build
make

# Install (optional)
sudo make install
```

### Verify Build

```bash
./kalidous --version
```

---

## Project Structure

```
Kalidous-Lang/
├── src/                      # Source code
│   ├── main.cpp
│   ├── lexer/
│   ├── parser/
│   ├── ast/
│   ├── type_checker/
│   ├── codegen/              # Bytecode generation
│   └── vm/                   # Virtual machine (under dev)
├── include/                  # Header files
├── tests/                    # Test suite
├── examples/                 # Example programs
├── docs/                     # Documentation (interactive HTML)
│   ├── index.html
│   ├── assets/
│   │   ├── css/
│   │   └── js/
│   ├── pages/
│   ├── sitemap.xml
│   └── robots.txt
├── CMakeLists.txt           # Build configuration
├── KalidousProject.toml      # Project metadata
├── install.sh               # Unix install script
├── install.ps1              # Windows install script
└── README.md                # This file
```

---

## Design Principles

### 1. Explicit & Clear

Code should be clear about what it does. Implicit behavior is minimized. Type annotations are available when needed.

### 2. Safe by Default

Memory safety, type safety, and error handling are enforced at compile time. Unsafe operations are explicit and well-documented.

### 3. Zero-Cost Abstractions

High-level language features compile to efficient machine code with no hidden overhead. What you write is what you get.

### 4. Modern Syntax

Inspired by contemporary language design best practices (Rust, Go, TypeScript), but tailored for Kalidous's philosophy.

### 5. Portable

KBC bytecode runs anywhere the Kalidous VM is available. Native compilation via LLVM will further expand portability.

---

## Contributing

Kalidous is a community project and welcomes contributions! Here's how you can help:

### Reporting Issues

Found a bug or have a feature request? [Open an issue](https://github.com/GalaxyHaze/Kalidous-Lang/issues) with:
- Clear description of the problem
- Steps to reproduce (if applicable)
- Expected vs actual behavior
- Your environment (OS, compiler, etc.)

### Contributing Code

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/your-feature`)
3. Make your changes
4. Add tests if applicable
5. Commit with clear messages (`git commit -m "Add feature: description"`)
6. Push to your fork and open a Pull Request

### Contribution Areas

- **Parser & Type System** - Enhance language features
- **Virtual Machine** - Improve bytecode execution
- **Standard Library** - Add useful utilities and modules
- **Documentation** - Improve docs and add examples
- **Testing** - Expand test coverage
- **Tooling** - Improve CLI and developer experience

### Code Guidelines

- Follow existing code style
- Add comments for complex logic
- Keep functions focused and small
- Write tests for new features
- Update documentation as needed

> **Disclaimer:** The API, syntax, and compiler behavior are subject to change frequently during this WIP phase.

---

## Community & Support

- **🐛 [Report Issues](https://github.com/GalaxyHaze/Kalidous-Lang/issues)** - Bug reports and feature requests
- **💬 [Discussions](https://github.com/GalaxyHaze/Kalidous-Lang/discussions)** - Questions, ideas, and community chat
- **📖 [Documentation](./docs/index.html)** - Comprehensive guides and language reference
- **🌟 Star the Project** - Show your support!

---

## Performance Goals

- **Fast Compilation** - Rapid feedback during development
- **Efficient Bytecode** - Small `.kbc` file sizes
- **Fast Execution** - Both in VM and native modes
- **Low Memory Usage** - Suitable for embedded systems (future goal)

---

## Frequently Asked Questions

**Q: When will Kalidous be production-ready?**
A: We're still in active development. The current focus is on completing the VM and standard library. Native compilation via LLVM will come after the VM is stable.

**Q: Can I use Kalidous for real projects?**
A: Not yet. It's a WIP. However, early adopters and testers are welcome!

**Q: What's the difference between KBC and native compilation?**
A: KBC bytecode runs in the Kalidous VM (portable, cross-platform). Native compilation (via LLVM) will produce machine code directly for maximum performance.

**Q: Can Kalidous code call C/C++ libraries?**
A: FFI support is planned but not yet implemented.

**Q: How do I contribute?**
A: See the [Contributing](#contributing) section above.

---

## License

Kalidous is licensed under the [MIT License](./LICENSE) - see the LICENSE file for details.

---

## Acknowledgments

- The Rust community for inspiring safe systems programming
- The LLVM project for providing excellent compiler infrastructure
- All contributors and early adopters

---

## Roadmap Timeline

- **Q2 2026** - Complete VM implementation
- **Q3 2026** - Standard library v1
- **Q4 2026** - LLVM backend integration
- **2027** - Language server and IDE support
- **2027+** - Production-ready release

---

**Made with ❤️ by the Kalidous Team**

*Last Updated: 2026-03-25*

---

### Quick Links
- 📖 [Documentation](./docs/index.html)
- 🐛 [Issues](https://github.com/GalaxyHaze/Kalidous-Lang/issues)
- 💬 [Discussions](https://github.com/GalaxyHaze/Kalidous-Lang/discussions)
- 🌟 [GitHub](https://github.com/GalaxyHaze/Kalidous-Lang)
