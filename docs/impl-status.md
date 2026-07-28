# Zith Implementation Status

> Last updated: 2026-07-15. Track live progress on the
> [Issue Tracker](https://github.com/GalaxyHaze/Zith/issues).

This document is the authoritative reference for what is currently working, what is parsed but
not yet semantically active, and what exists only at the specification level. All spec chapters
link here for status information so it does not need to be maintained in multiple places.

---

## Status Legend

| Label | Meaning |
|---|---|
| **Working** | Implemented, tested, and passes CTest. Safe to use. |
| **Partial** | Parsed and some lowering works; known gaps exist (see Notes). |
| **Blocked (E2010)** | Accepted by the parser; the semantic pass rejects it with error E2010 before it can reach HIR or codegen. Will be unlocked incrementally. |
| **Warn only** | Runs with a `NotImplemented` (W2009) warning; HIR lowering silently drops the node. |
| **Stub** | CLI command exists but returns "not implemented yet". |
| **Spec only** | Specified in the language docs; no compiler implementation exists. |

---

## Compiler Pipeline

| Stage | Status | Notes |
|---|---|---|
| Lexer | **Working** | Full token set including experimental keywords |
| Parser / scanner | **Working** | Full grammar. All AST nodes constructed correctly |
| Formatter | **Working** | Preserves all nodes including `SeqNode`, `WordCallNode`, `WordDeclNode`, `ContextDeclNode`, `UseNode` |
| Import resolution | **Working** | `import`, `from`, `export`, `alias` |
| Name resolution (sema) | **Working** | Symbol table, scoping, duplicate detection |
| Type checking | **Working** | Structs, enums, functions, generics, primitives |
| Generic instantiation (Solve) | Partial | Basic monomorphization works; complex constraint dispatch not yet |
| NRA pass | **Spec only** | Pipeline slot exists; ownership analysis not implemented |
| HIR lowering | **Working** | Covers all working features; unsupported nodes produce `kInvalidHirExpr` |
| LLVM codegen | **Working** | x86-64 and WebAssembly targets. Two-pass emission |
| Cache / `.zirl` output | Partial | Object caching works; `.zirl` library format not yet used |

---

## Language Features

### Core (Working)

| Feature | Notes |
|---|---|
| `fn`, `flow fn`, `const fn`, `raw fn` | All function kinds parse and lower |
| `struct`, `component`, `enum`, `union` | Full type definitions |
| `implement T {}` / `implement T as Trait {}` | Method and trait impl blocks |
| `trait`, `interface` | Declarations parse and resolve |
| Primitive types (`i8`-`i128`, `u8`-`u128`, `f32`, `f64`, `bool`, `char`) | Arithmetic, comparisons, unsigned ops |
| `let`, `var`, `const`, `global` | All binding forms |
| `if` / `when` (pattern matching) / `for` | Full control flow |
| `->` chain operator | Left-to-right pipelines |
| `import`, `from`, `export`, `alias`, `type` | Module system |
| `extern fn` | Manual C interop on every target |
| `pub` / `mod` / `mod(..)` / `mod(N)` | Visibility modifiers |
| `marker` / `dock` / `jump` | Structured goto inside `flow fn` |

### Partial / Warnings

| Feature | Status | Notes |
|---|---|---|
| C header imports (`import "header.h"`) | **Partial** | Native libclang only; C++ headers, macros, callbacks, variadics, and complex layouts are unsupported |

| Feature | Status | Notes |
|---|---|---|
| Field access (`.field`) | **Working** | Struct field reads/writes lower through HIR and LLVM |
| Index access (`a[i]`) | **Working** | Arrays, pointers, and indexed struct access lower through HIR and LLVM |
| Struct literals | Working | Positional and named forms work; named literals support explicit `_` defaults. Methods remain out of scope |
| Layout intrinsics (`@sizeOf`, `@alignOf`, `@offsetOf`) | Working | Backed by LLVM `DataLayout`; `@offsetOf` uses ABI layout offsets. |
| Range expressions | **Warn only** | W2009 |
| Other `@intrinsic` calls | **Warn only** | W2009 |
| Macro calls (`@macro`) | **Warn only** | W2009; no expansion |
| Generics (complex constraints) | Partial | Simple `<T>` works; `T: Trait or Other` constraints not fully enforced |
| `dyn` dispatch | Partial | Type annotation parsed; vtable lowering not complete |
| `async fn` / `yield` | **Spec only** | Parsed; sema does not typecheck async bodies |
| `spawn` / `await` | **Spec only** | Parsed; no thread model in HIR |

### Blocked — E2010 (Parsed, Rejected at Sema)

These are fully specified and parsed. They will be enabled incrementally once their HIR
representation and type semantics are finalized.

| Feature | Sema error | Next step |
|---|---|---|
| `is` expressions | E2010 | Requires union narrowing in HIR |
| `as` cast expressions | E2010 | Requires coercion rules in type checker |
| `?` / `!` postfix propagation | E2010 | Requires `?T` / `T!` types in HIR |
| `?` / `!` prefix fallback | E2010 | Requires `?T` / `T!` types in HIR |
| `word` / `nop` / `infix` declarations | E2010 | Requires word operator HIR nodes |
| `context` declarations | E2010 | Requires word/context resolution in sema |
| `use` statements | E2010 | Depends on context/word activation |
| Word call expressions | E2010 | Depends on word operator HIR nodes |
| Word sequence expressions (`SeqNode`) | E2010 | Depends on word operator HIR nodes |

### Spec Only (No Compiler Support)

| Feature | Spec chapter |
|---|---|
| NRA ownership analysis | [07-memory-model.md](07-memory-model.md) |
| `comptime` blocks / `const fn` evaluation | [11-comptime.md](11-comptime.md) |
| `async fn` / `yield` / `spawn` / `await` | [10-concurrency.md](10-concurrency.md) |
| `fail` blocks | [08-error-handling.md](08-error-handling.md) |
| `with` / `catch` | [08-error-handling.md](08-error-handling.md) |
| `must` / `throw` | [08-error-handling.md](08-error-handling.md) |
| Assets (`ZithProject.toml` asset paths) | [12-assets.md](12-assets.md) |
| Tag macros (`<Tag>`) | [15-macros.md](15-macros.md) |
| `.zirl` library distribution format | [01-overview (§1.5)](Zith-spec.md) |
| `@appendField`, `@removeField`, `@appendMethod` | [11-comptime.md](11-comptime.md) |

---

## CLI Commands

| Command | Status | Notes |
|---|---|---|
| `zithc build` | **Working** | |
| `zithc run` | **Working** | Compiles + executes in one step |
| `zithc check` | **Working** | Type-checks without emitting |
| `zithc fmt` | **Working** | Formats to `.clang-format`-style Zith conventions |
| `zithc create <name>` | **Working** | Scaffolds `ZithProject.toml` + `main.zith` |
| `zithc clean` | **Working** | Removes build dir and `.zith-cache` |
| `zithc execute <file>` | **Working** | Compiles and executes a single file |
| `zithc test` | **Stub** | Returns "not implemented yet" |
| `zithc repl` | **Stub** | Returns "not implemented yet" |
| `zithc deps` | **Stub** | Returns "not implemented yet" |

---

## Diagnostic Codes

| Code | Severity | Meaning |
|---|---|---|
| E2010 | Error | `UnsupportedSyntax` — feature parsed but not yet implemented in sema/HIR |
| W2009 | Warning | `NotImplemented` — feature parsed; HIR silently drops it |
| E2001 | Error | `UndefinedIdent` |
| E2002 | Error | `DuplicateDecl` |
| E2003 | Error | `WrongArity` |
| E2007 | Error | `NoMatchingFn` |
| E3001 | Error | `TypeMismatch` |

---

*This document is maintained alongside the compiler source. When a feature moves from Blocked to
Working, update this table and remove the corresponding E2010 rejection in `src/sema/sema-pipeline.cpp`.*
