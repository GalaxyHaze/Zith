# ZITH Language Specification
**Draft v0.9 — 2026**

> Zith is a statically typed systems programming language with a small, composable core and a large toolbox for domain-specific work. It proves memory safety at compile time without a garbage collector or borrow checker.

## Introduction

Zith gives you full control with a minimal & clean syntax — you don't have to choose between verbose but safe or readable but slow. Its memory model, Node Resource Analysis (NRA), proves ownership and lifetime safety using five keywords: `lend`, `view`, `unique`, `share`, and `belong` — plus a `default` (no keyword) modifier.

Beyond memory safety, Zith has a general-purpose core with a much larger toolbox: state machines, contexts (DSLs), words (custom operators), comptime. You choose when to use them. Zith also follows the **Rule of Three**: "if a function needs more than three specialized tools, something went wrong."

This document is a draft of the language specification, currently v0.9.
Not every feature described here is implemented in the compiler.
For the exact picture of what works today, see [Implementation Status](impl-status.md). It serves three audiences: developers learning Zith for the first time, contributors working on the `zithc` compiler, and tooling authors building editors, linters, or other infrastructure around the language.

### Notation used in this document

| Symbol | Meaning |
|---|---|
| `?T` | Optional type — `T` or `null` ([§8.1](08-error-handling.md#81-failable-types)) |
| `T!` | Result type — `T` or an error ([§8.1](08-error-handling.md#81-failable-types)) |
| `?` / `!` (postfix) | Unwrap an optional / result, propagating or falling back ([§8.3](08-error-handling.md#83-propagation--fallback)) |
| `@name` | Compiler intrinsic or macro invocation ([§11.3](11-comptime.md#113-reflection), [§15](15-macros.md)) |
| `#name` | Variable or field attribute, e.g. `#thread_local` or `#volatile` |
| `::` | Scope resolution — reach past a shadowed name ([§2.3](02-module-system.md#23-namespace-access--scope-resolution)) |

---

## 1. Overview & Design Philosophy

### 1.1 Who Zith Is For

If you are looking for just a 'new' language, clone or 'normal', so Zith is not for you. Zith was created for people starting to learn or open-minded, to discover new ways to think and structure your code, while having a powerful, readable, safe & expressive language.

### 1.2 Our Philosophy

Zith aims to be small and stable at its core — covering everyday needs — while offering a large kit that helps in specific domains where most languages need a lot of tricks to work.
The compiler is a copilot: it gives you the tools, and you build the systems.

| Everyday | Domain-specific |
|---|---|
| `struct`, `fn`, `lend`, `view`, `trait`, `interface` | `state`, `dock`, `jump` — for Games, State Machine, OS & embedded |
| `?T`, `T!`, `or` | `context`, `word` — for DSLs and APIs |
| `when`, `for`, `->` | runtime/stdlib concurrency APIs — for parallel work without special syntax |

### 1.3 Design Goals

- Expressive, minimal syntax that favors readability without sacrificing power.
- Memory safety via Node Resource Analysis (NRA).
- Composable behavior through traits, capabilities, and interfaces.
- Static, zero-overhead error handling with rich recovery semantics.
- Compile-time computation (`comptime`) as a first-class feature.
- Low-level control — state functions and musttail state transitions — without sacrificing safety in everyday code.
- Extensibility through words and macros, ideally scoped inside contexts rather than polluting the global namespace.

### 1.4 Context-Bound Extensibility (Best Practice)

Macros and words should ideally live inside a `context` block. Activating them globally is possible but discouraged — the same code smell as `using namespace std;` in C++.

```zith
// Preferred
use SQL {
    // SQL words and macros active only here
}

// Discouraged
use SQL;   // pollutes the rest of the file
```

### 1.5 Compilation Pipeline

The `zithc` compiler follows a multi-stage pipeline:

```
source -> lex -> scan -> resolve(import/symbols) -> sema -> comptime/solve -> NTA/NRA -> HIR -> LLVM
```
> Note: when you compile a library, after LLVM it outputs `.zirl` (Zith Intermediate Representation Library).

| Stage | Description |
|---|---|
| `source` | Receive arguments from CLI, load the file |
| `lex` | Tokenize the file into a `TokenStream` |
| `scan` | Find top-level declarations from the token stream |
| `resolve` | Resolve imported symbols, report duplicates |
| `sema` | Semantic analysis — name resolution, type checking, visibility |
| `comptime/solve` | Reserved for future generic instantiation, `comptime` evaluation, and the solved semantic view. Macro expansion currently happens during frontend parsing and uses the source AST directly |
| `NTA/NRA` | Accumulate semantic/resource facts, prove ownership rules, emit diagnostics, and apply only internal canonicalizations that do not change public ABI |
| `HIR` | Build High-level IR — the typed, desugared program with residual ownership facts attached when available |
| `LLVM` | Code generation via the LLVM backend |

`.zirl` files serve as cache and distribution format for compiled libraries — no headers needed, OS-agnostic, and you choose static or dynamic linking at the client side. Distribute once, link however the consumer prefers.

---

## Specification

| # | Topic | File | Summary |
|---|---|---|---|
| 2 | [Module System](02-module-system.md) | `02-module-system.md` | `import`, `from`, `export`, `alias`, `use`, visibility |
| 3 | [Type System](03-type-system.md) | `03-type-system.md` | Primitives, structs, enums, unions, generics, `when` |
| 4 | [Traits, Interfaces & Capabilities](04-traits-interfaces.md) | `04-traits-interfaces.md` | Nominal traits, structural interfaces, capabilities, operator overloading |
| 5 | [Functions](05-functions.md) | `05-functions.md` | `fn`, `const fn`, `state`, `raw fn`, `extern fn`, return types |
| 6 | [Mutability & Bindings](06-mutability-bindings.md) | `06-mutability-bindings.md` | `let`, `var`, `global`, `const`, deep mutability, destructuring |
| 7 | [Memory Model (NRA)](07-memory-model.md) | `07-memory-model.md` | Ownership, `lend`/`view`/`unique`/`share`/`belong`, the four rules |
| 8 | [Error Handling](08-error-handling.md) | `08-error-handling.md` | `?T`, `T!`, `with`/`catch`, `fail` blocks, `throw` |
| 9 | [Control Flow](09-control-flow.md) | `09-control-flow.md` | `if`, `when`, `for`, `->`, `state`, docks, jumps |
| 10 | [Concurrency & Runtime APIs](10-concurrency.md) | `10-concurrency.md` | stdlib/runtime concurrency surface, resource safety, no core syntax |
| 11 | [Comptime](11-comptime.md) | `11-comptime.md` | `const`, reflection, type manipulation, intrinsics |
| 12 | [Assets](12-assets.md) | `12-assets.md` | Compile-time asset processing, `ZithProject.toml` |
| 13 | [Raw & Unsafe](13-raw-unsafe.md) | `13-raw-unsafe.md` | `raw`, `unsafe`, `Trust` capability |
| 14 | [Polymorphism](14-polymorphism.md) | `14-polymorphism.md` | `dyn`, static vs dynamic dispatch, object safety |
| 15 | [Macros](15-macros.md) | `15-macros.md` | Scoped, raw, tag macros, `@` prefix, call-site scope behaviour |
| 16 | [Words](16-words.md) | `16-words.md` | Custom operators, `operator`, `token`, precedence |
| 17 | [Contexts](17-contexts.md) | `17-contexts.md` | DSL bundling, scoped activation |
| 18 | [C Interop](18-c-interop.md) | `18-c-interop.md` | `.h` import, manual binding, `extern 'C'` |
| 19 | [Project Configuration](19-project-config.md) | `19-project-config.md` | `ZithProject.toml`, `ZithFlags` |
| 20 | [Standard Library](20-standard-library.md) | `20-standard-library.md` | `std`, `soon`, `c` namespaces |
| 21 | [Best Practices](21-best-practices.md) | `21-best-practices.md` | Ownership patterns, naming conventions, Rule of Three |

---

## Appendix — Keyword & Operator Reference

### A.1 Keywords & Operators

| Keyword | Category | Summary |
|---|---|---|
| `import` / `from` / `export` | Module | Import / inject into scope / re-export. |
| `alias` | Module | Name alias for a type, namespace, or symbol. |
| `use` | Module | Bring a word, context, or operator into scope. |
| `type` | Types | Distinct type copy, or a compile-time constraint (with `or`). |
| `as` | Types | Cast / coercion. Also used in `implement T as Trait`. |
| `is` | Types | Type check / narrowing. Boolean. Supports `@struct`, `@nullable`, etc. |
| `enum` | Types | Closed compile-time constants — C-style, struct-backed, or ADT-style. |
| `union` | Types | Runtime tagged union; variants separated by commas. |
| `struct` | Types | Record type. Fields may be grouped with `[]`. |
| `component` | Types | POD / copy-by-default struct. No traits. C-compatible. |
| `implement` | Types | `implement T {}` or `implement T as Trait {}`. |
| `when` | Types | Pattern matching — ranges, type dispatch, branch tags, `..` to ignore fields. |
| `[]T` / `[N]T` / `[_]T` | Types | Slice / fixed array / deduced-size array. |
| `\| \|` | Types | Pack — named tuple / variadic / closure capture group. |
| `pub` / `mod` / `mod(..)` / `mod(N)` | Visibility | Public / module-local, with optional depth. |
| `let` / `var` / `global` / `const` | Bindings | Immutable / mutable / static storage / compile-time constant. |
| `default` / `lend` / `view` / `unique` / `share` / `belong` | Memory | NRA memory modifiers — `default` is implicit when no keyword is written. |
| `fn` / `const fn` / `state` / `raw fn` / `extern fn` | Functions | Five exclusive function kinds; cannot be combined. |
| `trait` / `interface` / `extends` / `requires` / `dyn` | OOP | Nominal traits, structural interfaces, extension, constraints, dynamic dispatch. |
| `Copy` / `Functor` / `Arithmetic` / `Error` | Capabilities | Operator and behavior capabilities. |
| `Null` / `Fail` | Capabilities | Negative — activate only in proven-invalid states. |
| `Allocator` / `Generator` / `Share` / `Lent` / `Trust` / `Unique` | Capabilities | Memory, runtime protocol, and safety capabilities. |
| `state` / `dock` / `jump` | State machines | `state` declarations, a state entry call, and terminating transitions. |
| `->` / `..` | Chain | Chain flow / placeholder for the previous value. Left-to-right. |
| `,` (in a chain) | Chain | Sub-chain — applies but does not advance the main chain value. |
| `operator` / `token` | Words | Custom operator definition / token word definition. Must be defined inside a `context`. |
| `?T` / `T!` | Errors | Optional / Result types. May be stacked. |
| `?` / `!` (postfix) | Errors | Propagate Option / Result. No semicolon. Propagate out of chains. |
| `or` | Errors / Loops / Types | Fallback / collapse an optional loop return / type constraint separator. |
| `must` | Errors | Panic in debug; guided removal in release. |
| `raw` | Errors / Raw | Always unchecked, in both debug and release. Compiler warns in release. |
| `unsafe` | Raw | Stronger than `raw`; valid only inside raw contexts. |
| `throw` / `fail` / `continue(v)` / `with` / `eager with` | Errors | Explicit throw, scoped recovery, resume, bundled fallible operations. |
| `::` | Operators | Scope resolution — access a shadowed outer name. |
| `and` / `or` / `not` / `xor` | Operators | Logical (English keywords). |
| `&.` / `\|.` / `^.` / `~` / `<<` / `>>` | Operators | Bitwise. |
| `@` / `#` | Annotations | `@` for intrinsics, reflection, and the macro prefix. `#` for variable/field attributes. |
| `extern 'C'` | Interop | C binding — automatic via `.h`, manual, or external. |
| `runtime` / `asm` | Config | `ZithProject` / `ZithFlags` build settings. |
| `assets` | Config | `ZithProject.toml` asset path declarations. |

### A.2 Compiler Intrinsics

| Intrinsic | Summary |
|---|---|
| `@fields T` | Iterate the fields of a type. |
| `@sizeOf T` | Size of a type, in bytes. |
| `@hasTrait T, Trait` | Check whether a type implements a trait. |
| `@struct` / `@component` / `@union` / `@enum` | Type-kind checks, used with `is`. |
| `@nullable` | Check whether a type is nullable (`?T`). |
| `@primitive` | Check whether a type is a primitive. |
| `@allocate T, data` | Allocate within the current memory region. |
| `@pack` | Extract or inject a closure's capture pack. |
| `@toStruct pack` | Convert a pack to a plain struct. |
| `@toPack struct, region` | Convert a struct back to a pack. |
| `@appendField Type, name: T` | Add a field to a type being constructed. |
| `@removeField Type, name` | Remove a field from a type being constructed. |
| `@appendMethod Type, fn ...` | Add a method to a type being constructed. |
| `@file` / `@line` / `@fnName` | Location information. |
| `@location` | Rich panic message source. |
| `@ok` / `@err` | Retrieve the T or E from `catch` & `fail`. |

### A.3 Attributes

| Attribute | Summary |
|---|---|
| `#volatile` | The variable is volatile; the compiler must not optimize it away. |
| `#thread_local` | The variable uses thread-local storage. |

---

*Zith Language Specification — Draft v0.9 — Subject to change*
