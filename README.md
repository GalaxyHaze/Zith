# Zith

[![Build](https://github.com/GalaxyHaze/Zith/actions/workflows/ci.yml/badge.svg)](https://github.com/GalaxyHaze/Zith/actions)
[![License](https://img.shields.io/github/license/GalaxyHaze/Zith)](./license)
[![Version](https://img.shields.io/github/v/release/GalaxyHaze/Zith)](https://github.com/GalaxyHaze/Zith/releases)
[![Discord](https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&logoColor=white)](https://discord.gg/a7h4cpWHg4)

> **Status: Early development.** Core types, functions, control flow, and LLVM codegen work.
> Several features listed below are designed and parsed but not yet semantically active.
> See the [Implementation Status](#implementation-status) table for the exact picture.

A statically typed, compiled, general-purpose language. Zith proves memory safety at compile time
through **Node Resource Analysis (NRA)** — no garbage collector, no borrow checker, no runtime
overhead. The syntax stays clean; the compiler does the hard work.

---

## What Makes Zith Different

NRA tracks ownership, lending, and aliasing through five qualifiers — `lend`, `view`, `unique`,
`share`, and `belong` — without requiring lifetime annotations. Beyond memory safety, Zith ships a
large toolbox for domain-specific work: custom operators (`word`), scoped DSLs (`context`), structured
goto (`flow fn` / `marker` / `dock`), and first-class compile-time computation.

---

## Quick Start

```bash
git clone https://github.com/GalaxyHaze/Zith.git
cd Zith
cmake -S . -B build
cmake --build build -j
./build/zithc --help
```

Requires **CMake 3.15+** and a **C++23** compiler (GCC 13+, Clang 16+, or MSVC 19.35+).
LLVM 18+ is optional — without it, codegen is disabled but `check` and HIR emission still work.

**Hello, World:** ( is a place holder, until we can formalize macros correctly)

```zith
extern fn puts(msg: *char)

fn main() {
    puts("Hello, World!");
}
```

then the canonical way would be:

```zith
from std/io/console

fn main() {
    @println("Hello, World!");
}
```


```bash
./build/zithc run examples/hello-world.zith
```

---

## Language Overview

**Type System**

- Primitives: `u8`-`u128`, `i8`-`i128`, `f32`, `f64`, `bool`, `char`, `void`
- Composite: `struct`, `component` (POD), `enum` (C-style, struct-backed, ADT), `union` (tagged & C-union hatch escape)
- Generics, `type` aliases, and pattern matching with `when`

**Memory Model (NRA)**

- `lend` — exclusive mutable borrow for the call
- `view` — read-only borrow
- `unique` — sole owner; value is consumed on assignment
- `share` — reference-counted, safe across scopes
- `belong` — declares that a field owns its pointee

**Functions**

- `fn` — regular function
- `const fn` — resolved entirely at compile time
- `flow fn` — structured goto with `marker` / `dock` / `jump` (no spaghetti, compiler-verified)
- `async fn` / `yield` — suspend/resume (parsed; semantic layer in progress)
- `raw fn` — opt out of NRA for C-interop

**Control Flow**

- `if` / `when` (pattern matching) / `for`
- `->` chain-flow operator for left-to-right pipelines

**Error Handling**

- `?T` optional, `T!` result — zero-cost, return-based
- `or` fallback chains, `fail` blocks, `with` / `catch`, `throw`
- `must` (debug panic) / `raw` (always unchecked)

**Extensibility (Experimental)**

- `word` — define custom infix, prefix, or suffix operators
- `context` — bundle words and macros into an activatable DSL scope
- `use` — bring a context or word into scope

---

## Implementation Status

| Feature | Status | Notes |
|---|---|---|
| Lexer / parser | **Working** | Full grammar including experimental syntax |
| Formatter (`zithc fmt`) | **Working** | Preserves all AST nodes including experimental |
| Type checking | **Working** | Structs, enums, generics, function calls |
| LLVM codegen | **Working** | x86-64 and WebAssembly targets |
| `fn`, `const fn`, `flow fn` | **Working** | |
| `struct`, `enum`, `union`, `component` | **Working** | |
| Primitive arithmetic and comparisons | **Working** | Including unsigned ops |
| `if` / `when` / `for` | **Working** | |
| `->` chain operator | **Working** | |
| Module imports (`import`, `from`, `export`) | **Working** | |
| `alias`, `type` | **Working** | |
| C interop (`extern fn`, `import ".h"`) | **Partial** | Manual `extern fn` works everywhere; `.h` imports require native libclang and support a restricted C ABI surface |
| Field access (`.field`) | Semantic warning | Parses, HIR lowering not yet complete |
| Index access (`a[i]`) | Semantic warning | Parses, HIR lowering not yet complete |
| `is` / `as` | **Blocked (E2010)** | Parsed; sema rejects before HIR |
| `?` / `!` propagation / fallback | **Blocked (E2010)** | Parsed; sema rejects before HIR |
| `word` / `context` / `use` | **Blocked (E2010)** | Parsed; sema rejects before HIR |
| `macro` / `@macro` calls | Semantic warning | Parses; expansion not implemented |
| `async fn` / `yield` / `spawn` / `await` | Spec only | No compiler support yet |
| NRA pass | Spec only | Pipeline stub exists; analysis not implemented |
| `comptime` blocks | Spec only | No evaluation yet |
| `zithc test` / `zithc repl` / `zithc deps` | Stub | Returns "not implemented" |

> **E2010 (UnsupportedSyntax):** the compiler accepts these constructs syntactically but emits a
> hard semantic error if they appear in code. They will be unlocked as their HIR representation
> and type semantics are fully defined.

---

## CLI Reference

| Command | Description | Status |
|---|---|---|
| `zithc build` | Compile to native binary | Working |
| `zithc run` | Compile and execute | Working |
| `zithc check` | Type-check without emitting | Working |
| `zithc fmt` | Format source files | Working |
| `zithc create <name>` | Scaffold a new project | Working |
| `zithc clean` | Remove build artifacts | Working |
| `zithc execute <file>` | Run a pre-compiled binary | Working |
| `zithc test` | Run project tests | Stub |
| `zithc repl` | Interactive REPL | Stub |
| `zithc deps` | Dependency management | Stub |

**Useful flags:** `--emit-ast`, `--emit-hir`, `--emit-ir`, `--emit-asm`, `-m release`, `--include <stdlib-path>`

---

## Compilation Pipeline

```
Source -> Lex -> Scan -> Import -> Resolve -> TypeCheck -> Solve -> NRA -> HIR -> Codegen -> Cache
```

| Stage | Description |
|---|---|
| `Lex` | Tokenize source |
| `Scan` | Register top-level declarations |
| `Import` | Resolve module imports |
| `Resolve` | Bind names to symbols |
| `TypeCheck` | Infer and check types (sema) |
| `Solve` | Generic instantiation, monomorphization |
| `NRA` | Node Resource Analysis — ownership pass (stub) |
| `HIR` | Lower typed AST to High-level IR |
| `Codegen` | Emit LLVM IR -> native or WASM binary |

---

## Install

**Build from source** (recommended until stable release):

```bash
git clone https://github.com/GalaxyHaze/Zith.git
cd Zith
cmake -S . -B build
cmake --build build -j
```

**Scoop (Windows):**
```powershell
scoop bucket add zithc https://github.com/GalaxyHaze/Zith.git
scoop install zithc
```

## Lexer Benchmark

The local lexer benchmark is opt-in and has no external benchmark dependency:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DZITH_BUILD_BENCHMARKS=ON
cmake --build build-release --target bench-lexer -j
./build-release/bench-lexer
./build-release/bench-lexer --format json
```

`bench-lexer` measures `lexer::tokenize` over the synthetic `mixed-valid` scenario. Source-map
file registration and host-information collection are outside the timed region; each tokenization
includes a new temporary arena, diagnostics state, and token stream allocation. Text and JSON
output identify the operating system, CPU model, logical CPU count, total memory, and compiler.
Compare results only on the same machine, with the same compiler and build flags. The JSON output
is intended for local automation and is not a versioned public API.

**Homebrew (macOS/Linux):**
```bash
brew tap galaxyhaze/zithc
brew install zithc
```

---

## Documentation

| Document | Purpose |
|---|---|
| [Language Spec](docs/Zith-spec.md) | Overview, design goals, quick reference, appendix |
| [Full Spec](docs/Zith-spec-full.md) | All chapters in one file |
| [CHANGELOG](CHANGELOG.md) | What changed and what is planned |
| [CONTRIBUTING](CONTRIBUTING.md) | Build instructions, code style, pipeline overview |

### Spec Chapters

| # | Topic | File |
|---|---|---|
| 1 | Overview & Design Philosophy | [Zith-spec.md](docs/Zith-spec.md) |
| 2 | Module System | [02-module-system.md](docs/02-module-system.md) |
| 3 | Type System | [03-type-system.md](docs/03-type-system.md) |
| 4 | Traits, Interfaces & Capabilities | [04-traits-interfaces.md](docs/04-traits-interfaces.md) |
| 5 | Functions | [05-functions.md](docs/05-functions.md) |
| 6 | Mutability & Bindings | [06-mutability-bindings.md](docs/06-mutability-bindings.md) |
| 7 | Memory Model (NRA) | [07-memory-model.md](docs/07-memory-model.md) |
| 8 | Error Handling | [08-error-handling.md](docs/08-error-handling.md) |
| 9 | Control Flow | [09-control-flow.md](docs/09-control-flow.md) |
| 10 | Concurrency | [10-concurrency.md](docs/10-concurrency.md) |
| 11 | Comptime | [11-comptime.md](docs/11-comptime.md) |
| 12 | Assets | [12-assets.md](docs/12-assets.md) |
| 13 | Raw & Unsafe | [13-raw-unsafe.md](docs/13-raw-unsafe.md) |
| 14 | Polymorphism | [14-polymorphism.md](docs/14-polymorphism.md) |
| 15 | Macros | [15-macros.md](docs/15-macros.md) |
| 16 | Words (Custom Operators) | [16-words.md](docs/16-words.md) |
| 17 | Contexts | [17-contexts.md](docs/17-contexts.md) |
| 18 | C Interop | [18-c-interop.md](docs/18-c-interop.md) |
| 19 | Project Configuration | [19-project-config.md](docs/19-project-config.md) |
| 20 | Standard Library | [20-standard-library.md](docs/20-standard-library.md) |
| 21 | Best Practices | [21-best-practices.md](docs/21-best-practices.md) |

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions and code style.

- [Discord](https://discord.gg/a7h4cpWHg4) — progress updates and collaboration
- [Issue Tracker](https://github.com/GalaxyHaze/Zith/issues)
- [Discussions](https://github.com/GalaxyHaze/Zith/discussions)

---

## License

[MIT License](./license)
