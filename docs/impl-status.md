# Zith Implementation Status

> Last updated: 2026-07-29 (23:59 UTC) (verified against `build/zithc check` on each feature).

This document is the single source of truth for what the compiler supports today. Every listed
feature was verified by running `build/zithc check` against a standalone test file; status
reflects actual compiler behaviour, not spec intent.

---

## Status Legend

| Label | Meaning |
|---|---|
| **Working**       | Accepted by parser and sema. Lowers through HIR to LLVM codegen. |
| **Check only**    | Passes `zithc check` but semantics are incidental (parsed as Name / Binary). No dedicated AST node, HIR, or codegen. |
| **Parse skipped** | Declaration accepted; body entirely skipped by `skipDelimited('{', '}')`. No semantics. |
| **Parse error**   | The parser itself rejects this construct. Does not reach sema. |
| **Spec only**     | No compiler implementation. |
| **Stub**          | CLI subcommand exists but returns "not implemented yet". |

---

## Compiler Pipeline

| Stage | Status | Notes |
|---|---|---|
| Lexer | **Working** | Hand-written, character-at-a-time. Maximal-munch for `==`, `!=`, `<=`, `>=`, `->`; other multi-char operators (`&&`, `+=`, `<<`, `..`) are still split per character |
| Parser | **Working** | Recursive-descent. Function decls, expressions, imports. |
| Formatter | **Working** | Round-trip stable for all 16 `ExprKind` nodes including `Index`, `OptionalProp`, `Field`, `Arrow`, and `StructLiteral` |
| Import resolution | **Working** | `import`, `from`, `export`, `alias`, `type` |
| Name resolution | **Working** | Scope-chained `lookupBinding`. Per-scope `DuplicateDecl`. |
| Type checking | **Working** | All `ExprKind` nodes. Optional/null validation. Index bounds. |
| Generic instantiation | Partial | Basic `<T>` works; `T: Trait` constraints not enforced |
| NRA pass | **Spec only** | Pipeline slot exists; ownership analysis not implemented |
| HIR lowering | **Working** | Covers all working features |
| LLVM codegen | **Working** | x86-64 and WebAssembly targets |
| Cache | Partial | Object caching works; `.zirl` format not yet used |

---

## Language Features

### Functions & Bindings

| Feature | Status | Notes |
|---|---|---|
| `fn` | **Working** | Parameters, return type, body |
| `flow fn` | **Working** | Parsed and lowers; `marker`/`dock`/`jump` not exhaustively tested |
| `raw fn` | **Working** | Parsed and lowers |
| `const fn` | **Parse error** | `const` is a binding keyword; `const fn f()` parses as `const` binding named `fn`, not a const function |
| `async fn` | **Parse skipped** | Parsed; body skipped via `skipDelimited`. No async lowering |
| `extern fn` | **Working** | C ABI interop |
| `let`, `var`, `const`, `global` | **Working** | All binding forms. `const` means immutable, not comptime |

### Types

| Feature | Status | Notes |
|---|---|---|
| `bool`, `char` | **Working** | |
| `i8`–`i128`, `u8`–`u128` | **Working** | Arithmetic between matching widths only; no implicit promotion |
| `f32`, `f64` | **Working** | Same-width arithmetic only |
| `?T` (optional) | **Working** | `null → ?T` and `T → ?T` coercions; `?` postfix propagation with operand/return validation. `null` is rejected for non-optional `*T` |
| `T!` (failable) | **Working** | Declared type; lowered through HIR |
| `*T` (pointer) | **Working** | Non-nullable: `null` requires `?*T`. `*p` deref, `&x` addr-of, and `->` arrow all work. `*void` is rejected (use `raw opaque`) |
| `[N]T` (array), `[]T` (slice) | **Working** | Indexing on arrays, slices, and pointers lowers through HIR/LLVM |
| `dyn Trait` | **Parse error** | Type parser does not handle `dyn` |
| `struct`, `component`, `enum`, `union` | **Working** | Declarations parse and resolve |
| `trait`, `interface` | **Working** | Declarations parse and resolve |
| `implement T as Trait {}` | **Working** | Method bodies lowered |
| `type` alias | **Working** | |

### Expressions

| Feature | Status | Notes |
|---|---|---|
| literals (`42`, `3.14`, `true`, `null`, strings) | **Working** | |
| unary `-`, `not` | **Working** | |
| binary `+` `-` `*` `/` `%` `==` `!=` `<` `>` `<=` `>=` | **Working** | |
| assignment `=` | **Working** | |
| field access `x.field` | **Working** | Dot access on struct values |
| dereference `*p` | **Working** | Pointer dereference via unary `*` |
| address-of `&x` | **Working** | Address-of via unary `&` |
| `->` chain operator | **Working** | Arrow access on struct pointers (`p->field`) |
| index `a[i]` | **Working** | On arrays, slices, pointers; rejects non-indexable types |
| `?` postfix propagation | **Working** | Requires optional operand in optional-returning function |
| `as` cast | **Working** | Dedicated `ExprKind::Cast` -> `HirCast` -> LLVM conversion. Numeric pairs only (`classifyCast`); pointer and user-defined casts rejected. No narrowing overflow check |
| `is null` | **Working** | Dedicated `ExprKind::IsNull`. Requires an optional operand; `?*T` uses the nullptr niche, `?T` reads the discriminant |
| `is <type>` | **Parse error** | Only `is null` is supported; any other operand reports a dedicated diagnostic |
| range `1..5` | **Check only** | Parsed as binary `..`; no dedicated sema |
| struct literal `Foo { x: 1, y: 2 }` | **Working** | Struct literal with named fields via `{}` syntax |
| `@sizeOf`, `@intrinsic` | **Parse error** | `@` in expression position not handled |

### Control Flow

| Feature | Status | Notes |
|---|---|---|
| `if` / `else` / `else if` | **Working** | |
| `while` | **Deprecated** | Still lowers correctly, but emits `W1008` suggesting `for (cond) { }` |
| `break`, `continue` | **Working** | |
| `return` (void and typed) | **Working** | |
| `for (cond) { }`, `for { }` | **Working** | Conditional and infinite loop forms lower to the same CFG as `while` |
| `for (x in xs)`, `for (init), (cond), (step)` | **Parse error** | Recognised and reported as not implemented yet |
| `when` pattern match | **Parse error** | `when (x)` partially parsed; arm syntax `0 => { }` unrecognised |
| `marker` / `dock` / `jump` | **Check only** | Parsed in `flow fn`; lowering not verified |

### Words, Contexts, Macros

| Feature | Status | Notes |
|---|---|---|
| `prefix`, `suffix`, `infix`, `nop` decls | **Parse skipped** | Body skipped via `skipDelimited` |
| `context` declarations | **Parse skipped** | Body skipped |
| `use` statements | **Parse skipped** | Body skipped |
| `@macro` calls | **Parse error** | `@` in expression position not handled |
| word call expressions | **Parse error** | No parser support |
| word sequence expressions | **Parse error** | No parser support |

### Module System & Visibility

| Feature | Status | Notes |
|---|---|---|
| `import`, `from`, `export` | **Working** | Import resolution with correct paths |
| `alias` | **Working** | |
| `pub`, `mod` | **Working** | |
| `mod(..)`, `mod(N)` | Not verified | Parser accepts; sema behaviour unknown |
| C header imports | Partial | libclang only; macros, callbacks, variadics, complex layouts unsupported |

---

### Spec Only (No Compiler Support)

| Feature | Spec chapter |
|---|---|
| NRA ownership analysis (`lend`, `view`, `unique`, `share`, `belong`) | [07-memory-model.md](07-memory-model.md) |
| `comptime` evaluation | [11-comptime.md](11-comptime.md) |
| `const fn` evaluation | [11-comptime.md](11-comptime.md) |
| `yield` | [10-concurrency.md](10-concurrency.md) |
| `spawn`, `await` | [10-concurrency.md](10-concurrency.md) |
| `async fn` lowering | [10-concurrency.md](10-concurrency.md) |
| `fail` / `with` / `catch` / `must` / `throw` | [08-error-handling.md](08-error-handling.md) |
| Assets (`ZithProject.toml` asset paths) | [12-assets.md](12-assets.md) |
| Tag macros (`<Tag>`) | [15-macros.md](15-macros.md) |
| `.zirl` binary format | [01-overview (§1.5)](Zith-spec.md) |
| `@appendField`, `@removeField`, `@appendMethod` | [11-comptime.md](11-comptime.md) |
| `when` pattern matching | [09-control-flow.md](09-control-flow.md) |
| `match` | [09-control-flow.md](09-control-flow.md) |
| `dyn` dispatch | [14-polymorphism.md](14-polymorphism.md) |

---

## CLI Commands

| Command | Status | Notes |
|---|---|---|
| `zithc build` | **Working** | |
| `zithc run` | **Working** | Compiles + executes in one step |
| `zithc check` | **Working** | Type-checks without emitting. Errors forwarded from frontend snapshot |
| `zithc fmt` | **Working** | Round-trip tested for `Index` and `OptionalProp` |
| `zithc create <name>` | **Working** | |
| `zithc clean` | **Working** | |
| `zithc execute <file>` | **Working** | |
| `zithc test` | **Stub** | |
| `zithc repl` | **Stub** | |
| `zithc deps` | **Stub** | |

---

## Diagnostic Codes

| Code | Severity | Meaning |
|---|---|---|
| E0001 | Error | Parse error |
| E0000 | Error | User-reported diagnostic (type mismatch, optional validation, etc.) |
| E2002 | Error | `DuplicateDecl` — duplicate binding in same scope |
| E1006 | Error | Import resolution failure |
| E3003 | Error | `InvalidCast` — non-numeric `as` conversion |
| W1008 | Warning | `DeprecatedSyntax` — `while` should be written `for (cond)` |

---

## Verification

All statuses above were verified against the current `build/zithc` binary (2026-07-29):

```bash
build/zithc --include stdlib check examples/hello-world.zith   # [ok] check passed
build/zithc run examples/linked-list.zith                        # exit 7
ctest --test-dir build                                           # 23/23 passed
```

---

## Known Debt

Recorded deliberately; each item is a follow-up, not an unknown.

| Item | Notes |
|---|---|
| Formatter re-prints `for (cond)` as `while` | `for` reuses `ExprKind::While`; a distinct node is needed to round-trip the spelling |
| No overflow check on narrowing conversions | Neither `as` nor numeric-literal adaptation validates that the value fits the target |
| No flow-sensitive narrowing after `is null` | `p->field` on a `?*T` is accepted without proving the pointer is non-null |
| `is` limited to `is null` | Union/type narrowing is not addressed |
| `for` iterator and 3-clause forms unimplemented | Reported as errors rather than parsed |
| User-defined casts | To be added as a new branch in `classifyCast` |
| Multi-char operators beyond the five | `&&`, `\|\|`, `+=`, `<<`, `>>`, `..` still lex per character (their `precedence()` is -1) |

*When a feature moves from one status to another, update this table and re-verify.*
