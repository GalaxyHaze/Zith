# Kalidous Language

> **WIP:** Kalidous is currently in active development. While the core concepts are being defined, the project is not yet stable or ready for production use.

## About

Kalidous is a general-purpose programming language designed to bridge the gap between high-level developer ergonomics and low-level system performance.

It achieves this by transpiling to Zig, effectively leveraging Zig's powerful compiler backend while offering a modern, distinct syntax and feature set tailored for safety and speed.

## Current Status

We are currently in the early stages of development. Here is a snapshot of the project's progress:

### Completed
- **File Handling:** Reliable source code ingestion and file management.
- **Tokenizer:** A fully functional lexical analyzer that breaks source code into tokens for processing.

### In Progress
- **CLI Interface:** We are currently building out the command-line interface to handle file execution, compilation, and project initialization.

### Roadmap
- Parser & AST construction
- Zig Transpilation engine
- Standard Library foundations

## Design Philosophy

Kalidous is not just "Zig with different syntax." It aims to inherit the raw power of Zig while introducing new capabilities that make the compiler and the programmer work as a team.

### 1. Deep Integration with Zig
By targeting Zig as an intermediate language, Kalidous inherits the battle-tested performance and modern tooling of the Zig ecosystem. This allows us to focus on language design while relying on Zig for optimal machine code generation.

### 2. Compilation Capabilities
One of the core goals of Kalidous is to unlock deep Compile-Time (comptime) capabilities. We intend to provide features where the programmer and the compiler communicate effectively, allowing for highly optimized code generation and advanced reflection techniques that feel natural rather than hacky.

### 3. Safety & Memory Management
Performance should not come at the cost of safety. Kalidous is designed with a memory management model that prioritizes both Safety (preventing common vulnerabilities) and Speed (minimal runtime overhead), ensuring developers can write fast code without fear of memory corruption.

## Installation

> **Note:** The CLI is currently under heavy development. Installers are provided for early testing and feedback.

### Linux / macOS

Run the following command in your terminal:

```bash
curl -sSL https://raw.githubusercontent.com/GalaxyHaze/Kalidous/master/install.sh | bash
```

To install a specific version:
```bash
curl -sSL https://raw.githubusercontent.com/GalaxyHaze/Kalidous/master/install.sh | bash -s -- v.alpha-0.3
```

### Windows

Run the following command in PowerShell:

```powershell
irm https://raw.githubusercontent.com/GalaxyHaze/Kalidous/master/install.ps1 | iex
```

To install a specific version:
```powershell
$env:KALIDOUS_VERSION="v.alpha-0.3"; irm https://raw.githubusercontent.com/GalaxyHaze/Kalidous/master/install.ps1 | iex
```

## Contributing

Since this is a Work In Progress, contributions are valuable! Whether it's reporting bugs, discussing syntax ideas, or submitting code, we appreciate the help.

> **Disclaimer:** The API, syntax, and compiler behavior are subject to change frequently during this phase.
```
