# Contributing to Zith

## Building

```bash
cmake -B build
cmake --build build
```

Requires C++23, CMake 3.20+, and LLVM (for codegen).

## Project structure

```
src/
  memory/       - Arena allocator, DynArray, FlatMap, utilities
  ast/          - ID-based AST data model and builder
  lexer/        - Tokenizer
  parser/       - Recursive-descent + Pratt parser
  diagnostics/  - Error reporting, diagnostic engine
  symbols/      - Symbol table, import resolution, name resolver
  types/        - Type intern, unification, lowering
  sema/         - Semantic analysis, type checking, NRA preparation, HIR lowering
  hir/          - High-level IR (typed, lowered from AST)
  comptime/     - Compile-time evaluation, monomorphization
  codegen/      - LLVM IR codegen
  output/       - Binary sectioned format (.irl/.mod)
  cache/        - Incremental compilation
  formatter/    - Code formatter
  cli/          - CLI argument parsing, command dispatch
  session/      - Per-file compilation session, pipeline orchestration
  capi/         - C ABI wrapper
  support/      - Platform compatibility
```

## Code style

- clang-format (see `.clang-format` at root)
- Namespaces match directory names under `src/`
- Arena-backed allocation throughout
- No exceptions, no RTTI

## Pipeline

```
Source → Lex → Scan → Import → Resolve → Sema → Comptime/Solve → NTA/NRA → HIR → Codegen → Cache
```

Each stage is a method on `CompilationSession` in `src/session/`.

## Implementation Status

See [docs/impl-status.md](docs/impl-status.md) for the current state of every language feature and CLI command.
