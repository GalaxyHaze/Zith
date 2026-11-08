# ZITH Language Specification
**Draft v0.9: 2026**

> **Status:** this file is the design reference for Zith, a sibling language to
> `Zith--`. The languages have separate contracts. This specification is
> neither a superset of `Zith--` nor a roadmap for its evolution. The `zithc`
> compiler contract for `Zith--` is [Zith--.md](Zith--.md), with implementation
> status in [impl-status.md](impl-status.md).

> This document describes Zith only. Features described here are not claims
> about `Zith--`. The status of the `Zith--` compiler does not establish an
> implementation status for Zith.

> Zith is a statically typed systems programming language with a small, composable core and a large toolbox for domain-specific work. It proves memory safety at compile time without a garbage collector or borrow checker.

## Introduction

Zith gives you full control with a minimal & clean syntax — you don't have to choose between verbose but safe or readable but slow. Its memory model, Node Resource Analysis (NRA), proves ownership and lifetime safety. Zith uses `&T` and `&mut T` references, `^T` binds, and `%T` owned values, with an implicit `default` form. Zith-- has its own `lend`/`view` call-annotation model; see [Implementation Status](impl-status.md) for that language's behavior.

Beyond memory safety, Zith has a general-purpose core with a larger toolbox: state machines, contexts for domain-specific syntax, words (custom operators), and comptime. You choose when to use them. Zith also follows the **Rule of Three**: "if a function needs more than three specialized tools, something went wrong."

Zith also aims for a shared public-API style that remains familiar across
different implementation styles. A project's language identity is separate:
it controls optional features and local diagnostic rationale for the project's
own code, not the design of public APIs or the features used by dependencies.
See [ADR 0032](adr/0032-universal-api-project-identity-and-contexts.md).

This document is a draft of the Zith language specification, currently v0.9.
It does not specify the language implemented by the current `zithc` compiler.
For the exact picture of what `zithc` supports for sibling language `Zith--`,
see [Implementation Status](impl-status.md). That document makes no claim
about Zith implementation status.

### Notation used in this document

| Symbol | Meaning |
|---|---|
| `?T` | Deprecated optional type wrapper. Still accepted by the current Zith-- compiler as legacy syntax. |
| `T!` | Deprecated as a type wrapper. In a function return annotation, `T!` declares success type `T` and lets the compiler infer invalid states. |
| `try` / `or` | Error handling | Short-circuit one expression and provide fallbacks for any invalid state ([§8.3](08-error-handling.md#83-try-propagation-and-fallback)). |
| Postfix `?` | Legacy Zith-- syntax | Propagates `Nil`; its current compiler support is separate from Zith's error propagation model. |
| Postfix `!` | Error handling | Propagate an operation's invalid state to the enclosing function ([§8.3](08-error-handling.md#83-try-propagation-and-fallback)). |
| `@name` | Compiler intrinsic or compiler magic. Zith-- also uses it for macro calls. |
| `@<Tag>` | Dedicated full-Zith tag opener, closed by `</Tag>`. |
| `#name` | Variable or field attribute, e.g. `#thread_local` or `#volatile` |
| `::` | Scope resolution — reach past a shadowed name ([§2.3](02-module-system.md#23-namespace-access--scope-resolution)) |

---

## 1. Overview & Design Philosophy

### 1.1 Who Zith Is For

If you are looking for just a 'new' language, clone or 'normal', so Zith is not for you. Zith was created for people starting to learn or open-minded, to discover new ways to think and structure your code, while having a powerful, readable, safe & expressive language.

### 1.2 Our Philosophy

Zith aims to be small and stable at its core, covering everyday needs, while offering a large kit that helps in specific domains where most languages need a lot of tricks to work.
The compiler is a copilot: it gives you the tools, and you build the systems.

| Everyday | Domain-specific |
|---|---|
| `struct`, `fn`, `&`, `&mut`, `trait`, `contract` | `state`, `dock`, `jump` — for Games, State Machine, OS & embedded |
| `?T`, `or` | `context`, `word` — for domain-specific syntax integration |
| `when`, `for`, `->` | runtime/stdlib concurrency APIs — for parallel work without special syntax |

### 1.3 Design Goals

- Expressive, minimal syntax that favors readability without sacrificing power.
- Memory safety via Node Resource Analysis (NRA).
- Composable behavior through traits, capabilities, and structural contracts.
- Static, zero-overhead error handling with rich recovery semantics.
- Compile-time computation (`comptime`) as a first-class feature.
- Low-level control, such as state functions and musttail state transitions, without sacrificing safety in everyday code.
- Domain-specific syntax integration through contexts, tags, and words. Contexts are not general-purpose API namespaces.

### 1.4 Domain-Specific Syntax Integration

Contexts are reserved for APIs that deliberately integrate with a domain's
syntax, such as SQL, HTML, or Math. They are not generic containers for public
API declarations or general-purpose syntax declarations. Prefer scoped
activation when a domain integration only applies to one block. Full-Zith tag
delimiters and body kinds are defined in
[ADR 0032](adr/0032-universal-api-project-identity-and-contexts.md). Exact
context activation and tag declaration syntax remain under design.

```zith
// Domain-specific syntax, scoped to this block
use SQL {
    // SQL-specific forms are active here
}

// The current draft also permits activation in the surrounding scope
use SQL;
```

### 1.5 Compilation Pipeline

The `zithc` compiler follows a multi-stage pipeline:

```
source -> lex -> scan -> resolve(import/symbols) -> sema -> comptime/solve -> SRA -> HIR -> LLVM
```
> Note: when you compile a library, after LLVM it outputs `.zirl` (Zith Intermediate Representation Library).

| Stage | Description |
|---|---|
| `source` | Receive arguments from CLI, load the file |
| `lex` | Tokenize the file into a `TokenStream` |
| `scan` | Find top-level declarations from the token stream |
| `resolve` | Resolve imported symbols, report duplicates |
| `sema` | Semantic analysis — name resolution, type checking, visibility |
| `comptime/solve` | Reserved for future generic instantiation, `comptime` evaluation, and the solved semantic view. In Zith--, macro expansion currently happens during frontend parsing and uses the source AST directly. |
| `SRA` | Accumulate residual resource facts, prove the ownership slice, emit diagnostics, and apply only internal canonicalizations that do not change public ABI |
| `HIR` | Build High-level IR — the typed, desugared program with residual ownership facts attached when available |
| `LLVM` | Code generation via the LLVM backend |

`.zirl` files serve as cache and distribution format for compiled libraries — no headers needed, OS-agnostic, and you choose static or dynamic linking at the client side. Distribute once, link however the consumer prefers.

---

## Specification

The chapters below describe the Zith language design. Zith and `Zith--` are
sibling languages with separate contracts, so features described here do not
define `Zith--` or imply that it will grow into Zith. `string`, `T!` failable
values, `&T`/`&mut T`, `^T`, `%T`, `try`/`fail`/`catch`, `use`/contexts/words,
and `tag` items appear here as Zith design material. For current `zithc`
behavior, read [`Zith--.md`](Zith--.md) and [`impl-status.md`](impl-status.md).

| # | Topic | File | Summary |
|---|---|---|---|
| 2 | [Module System](02-module-system.md) | `02-module-system.md` | `import`, `from`, `export`, `alias`, `use`, visibility |
| 3 | [Type System](03-type-system.md) | `03-type-system.md` | Primitives, structs, enums, unions, generics, `when` |
| 4 | [Traits, Contracts & Capabilities](04-traits-interfaces.md) | `04-traits-interfaces.md` | Nominal traits and composition, static structural contracts, capabilities |
| 5 | [Functions](05-functions.md) | `05-functions.md` | `fn`, `const fn`, `state`, `raw fn`, `extern fn`, return types |
| 6 | [Mutability & Bindings](06-mutability-bindings.md) | `06-mutability-bindings.md` | `let`, `var`, `global`, `const`, deep mutability, destructuring |
| 7 | [Memory Model (NRA)](07-memory-model.md) | `07-memory-model.md` | Zith ownership through `&`/`&mut`, `^`, and `%`, plus the four rules. Zith-- separately defines `lend`/`view` call annotations. |
| 8 | [Error Handling](08-error-handling.md) | `08-error-handling.md` | `Failable` / `Invalid`, inferred invalid states, `try` / `or`, `fail`, `must` |
| 9 | [Control Flow](09-control-flow.md) | `09-control-flow.md` | `if`, `when`, `for`, `->`, `state`, docks, jumps |
| 10 | [Concurrency & Runtime APIs](10-concurrency.md) | `10-concurrency.md` | stdlib/runtime concurrency surface, resource safety, no core syntax |
| 11 | [Comptime](11-comptime.md) | `11-comptime.md` | `const`, reflection, type manipulation, intrinsics |
| 12 | [Assets](12-assets.md) | `12-assets.md` | Compile-time asset processing, `ZithProject.toml` |
| 13 | [Raw & Unsafe](13-raw-unsafe.md) | `13-raw-unsafe.md` | `raw`, `unsafe`; full Zith uses `safe`/`trust`/`hazard` ([ADR-0043](adr/0043-full-zith-safety-levels-safe-trust-hazard.md)) |
| 14 | [Polymorphism](14-polymorphism.md) | `14-polymorphism.md` | `dyn`, static vs dynamic dispatch, object safety |
| 15 | [Macros](15-macros.md) | `15-macros.md` | Zith-- normal and raw macros, full-Zith tags, and their distinct syntax |
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
| `as` | Types | Cast / coercion. |
| `is` | Types | Type check / narrowing. Boolean. Supports `@struct`, `@nullable`, etc. |
| `enum` | Types | Closed compile-time constants with one declared value type, or heterogeneous values through `enum: union`. |
| `union` | Types | Runtime-tagged value holding one of its declared member types, which may be heterogeneous. |
| `struct` | Types | Record type. Fields may be grouped with `[]`. |
| `pod` | Types | POD / copy-by-default struct. No traits. C-compatible. |
| `extend` | Types | Declares nominal trait conformance with `extend T with Trait {}`. |
| `when` | Types | Pattern matching — ranges, type dispatch, branch tags, `..` to ignore fields. |
| `[]T` / `[N]T` / `[_]T` | Types | Slice / fixed array / deduced-size array. |
| `\| \|` | Types | Pack — named tuple / variadic / closure capture group. |
| `pub` / `mod` / `mod(..)` / `mod(N)` | Visibility | Public / module-local, with optional depth. |
| `let` / `var` / `global` / `const` | Bindings | Immutable / mutable / static storage / compile-time constant. |
| `default` / `&T` / `&mut T` / `^T` / `%T` | Memory | Zith reference and ownership forms. Zith-- separately defines `lend`/`view` call annotations. |
| `fn` / `const fn` / `state` / `extern fn` | Functions | Exclusive function kinds; cannot be combined. Safety is a separate level (`safe`/`trust`/`hazard`, [ADR-0043](adr/0043-full-zith-safety-levels-safe-trust-hazard.md)), not a function kind. |
| `trait` / `contract` / `extends` / `requires` / `dyn` | OOP | Nominal traits, trait composition, structural contracts, bounds, and trait dynamic dispatch. |
| `Copy` / `Functor` / `Arithmetic` | Capabilities | Operator and behavior capabilities. |
| `Failable` / `Invalid` / `Error` | Capabilities | Invalid-state analysis, methods on proven-invalid `Failable` values, and the marker capability for error values ([§8](08-error-handling.md)). |
| `Allocator` / `Generator` / `Share` / `Lent` / `Unique` | Capabilities | Memory and runtime-protocol capabilities. The `Trust` capability is removed in full Zith and replaced by the `trust fn` safety level ([ADR-0043](adr/0043-full-zith-safety-levels-safe-trust-hazard.md)). |
| `state` / `dock` / `jump` | State machines | `state` declarations, a state entry call, and terminating transitions. |
| `fork` / `merge` / `revoke` | Threads | Core full-Zith syntax: create a thread, collect results, and revoke child access to resources. |
| `spawn` | Threads | Stdlib shorthand for an implicit fork; not a core keyword. |
| `->` / `..` | Chain | Chain flow / placeholder for the previous value. Left-to-right. |
| `,` (in a chain) | Chain | Sub-chain — applies but does not advance the main chain value. |
| `operator` / `token` | Words | Custom operator definition / token word definition. Must be defined inside a `context`. |
| `?T` / `T!` | Errors | Deprecated as type wrappers. `T!` remains a function return annotation for inferred invalid states. |
| `try` / `or` | Errors | `try` yields a local, bindable valid/invalid result; `or` evaluates a fallback only after invalidity and retains the last invalid result. |
| Postfix `?` | Errors | Legacy Zith-- Nil propagation. |
| Postfix `!` | Errors | Propagate an operation's invalid state to the enclosing function. |
| `or` | Errors / Loops / Types | Invalid-state fallback / loop fallback / type constraint separator. |
| `must` / `assert` | Errors | `must` guards a failable value; `assert` checks a boolean condition. They are distinct. |
| `raw` | Errors / Raw | Always unchecked, in both debug and release. Compiler warns in release. |
| `unsafe` | Raw | Stronger than `raw`; valid only inside raw contexts. |
| `throw` / `fail` / `resume` | Errors | `fail` captures invalid values whose types implement `Error`; `resume x;` replaces the failed result. |
| `with` / `catch` | Errors | `catch` captures any original invalid value from `with` initialization, not errors from its body. |
| `::` | Operators | Scope resolution — access a shadowed outer name. |
| `and` / `or` / `not` / `xor` | Operators | Logical (English keywords). |
| `&.` / `\|.` / `^.` / `~` / `<<` / `>>` | Operators | Bitwise. |
| `@` / `@<` / `#` | Compiler syntax | `@` for intrinsics and compiler magic, `@<` for full-Zith tags. Zith-- also uses `@` for macro calls. `#` marks variable and field attributes. |
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
| `@ok` | Used with `is` to narrow a `Failable` value to its valid state; the `else` branch proves it invalid. |

The `@component` type-kind predicate remains a current spelling. Whether it
changes to `@pod` with the full-Zith `pod` declaration is still open.

### A.3 Attributes

Full-Zith capability-use and purity annotations are static contracts. Their
semantics are recorded in
[ADR-0039](adr/0039-full-zith-capability-and-purity-contracts.md). Their
canonical placement and ordering are fixed by
[ADR-0044](adr/0044-full-zith-annotation-order-and-purity-rename.md). Zith--
retains its existing syntax.

| Attribute | Summary |
|---|---|
| `#volatile` | The variable is volatile; the compiler must not optimize it away. |
| `#thread_local` | The variable uses thread-local storage. |
| `@using(X, Y)` | Requires at least one reachable use of each named capability. Other capabilities remain allowed. |
| `@only(X, Y)` | Allows only the listed capabilities. `@only(not X)` prohibits `X`. |
| `@pure` | Requires deterministic behavior with no side effects. |
| `@honest` | Requires the returned value to depend only on explicit arguments. Other side effects may remain. Formerly `@valuePure`. |
| `@impure` | Requires the function to be classified as impure. |
| `safe` / `trust` / `hazard` | Safety level of a callable. See [ADR-0043](adr/0043-full-zith-safety-levels-safe-trust-hazard.md). |

Full-Zith keyword spellings and the eight-byte limit are recorded in
[ADR-0040](adr/0040-full-zith-surface-names-and-keyword-limit.md). That
decision does not change the `component`, `interface`, or `implement`
spellings accepted by Zith--.

---

*Zith Language Specification — Draft v0.9 — Subject to change*
