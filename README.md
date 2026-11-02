# Zith

[![Build](https://github.com/GalaxyHaze/Zith/actions/workflows/ci.yml/badge.svg)](https://github.com/GalaxyHaze/Zith/actions)
[![License](https://img.shields.io/github/license/GalaxyHaze/Zith)](#license)
[![Discord](https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&logoColor=white)](https://discord.gg/a7h4cpWHg4)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/CMake-3.20%2B-red)](https://cmake.org/)

A systems language that lets you choose how you will implement a task.
Follow idiomatic Zith-style, C-style, functional, or whatever fits your team.

The current compiler implements **Zith--**, a working subset of the full language.
See the [Zith-- specification](docs/Zith--.md) and the
[implementation status](docs/impl-status.md) for what works today.

## Quick Start

```bash
git clone https://github.com/GalaxyHaze/Zith.git
cd Zith
cmake -S . -B build
cmake --build build -j
./build/zithc --help
```

Requires **CMake 3.20+**, a **C++23** compiler, and LLVM 18+ for native codegen.

## Compilation Pipeline

The compiler moves source through these stages:

```text
Lexer -> Parser -> Formatter -> Import resolution -> Name resolution -> Type checking -> Generic instantiation -> Comptime / Solve -> SRA / Reference Analysis -> HIR lowering -> LLVM codegen -> Cache
```

| Stage | Purpose |
|---|---|
| Lexer | Converts source characters into tokens |
| Parser | Builds frontend declarations and expressions |
| Formatter | Produces stable source formatting |
| Import resolution | Loads imported modules |
| Name resolution | Resolves symbols and scopes |
| Type checking | Validates expression and declaration types |
| Generic instantiation | Monomorphizes generic declarations |
| Comptime / Solve | Runs compile-time work |
| SRA / Reference Analysis | Tracks the implemented ownership facts |
| HIR lowering | Lowers checked programs into HIR |
| LLVM codegen | Emits native or WebAssembly code |
| Cache | Persists and reuses compiled artifacts |

## Documentation

| Document | Purpose |
|---|---|
| [Zith--.md](docs/Zith--.md) | Current compiler subset specification |
| [impl-status.md](docs/impl-status.md) | Verified status of every feature and command |
| [CHANGELOG.md](CHANGELOG.md) | What changed and what is planned |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Build instructions, code style, and pipeline overview |

## CLI Reference

| Command | Status | Notes |
|---|---|---|
| `zithc build` | **Working** | Compiles a project or source file |
| `zithc run` | **Working** | Compiles and runs a program |
| `zithc check` | **Working** | Type-checks without emitting |
| `zithc fmt` | **Working** | Formats source code |
| `zithc create <name>` | **Working** | Creates a project |
| `zithc clean` | **Working** | Removes build artifacts |
| `zithc execute <file>` | **Working** | Executes a compiled artifact |
| `zithc test <path>` | **Working** | Runs discovered `#[test]` functions |
| `zithc repl` | **Stub** | |
| `zithc deps list` | **Working** | Lists project dependencies |
| `zithc deps add <name> <relative-path>` | **Working** | Adds a local path dependency |
| `zithc deps remove <name>` | **Working** | Removes a dependency declaration |
| `zithc deps resolve` | **Working** | Resolves local dependencies into a deterministic `Zith.lock` |
| `zithc docs` | **Working** | Generates Markdown documentation |
| `zithc completion <shell>` | **Working** | Emits shell completion scripts |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions and code style.
Maintainers can publish and verify the Homebrew tap with the
[Homebrew publication wizard](scripts/publish-homebrew-tap.sh).

- [Discord](https://discord.gg/a7h4cpWHg4) — progress updates and collaboration
- [Issue Tracker](https://github.com/GalaxyHaze/Zith/issues)
- [Discussions](https://github.com/GalaxyHaze/Zith/discussions)

## License

[MIT License](./license)
