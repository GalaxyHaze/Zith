# Zith Implementation Status

> Last updated: 2026-08-06 (documentation architecture refresh).

This document is the single source of truth for what the compiler supports today. Status reflects actual compiler behaviour at baseline `bf7925e`, verified by direct source-code inspection.
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
| **In progress (uncommitted)** | AST nodes, parser handlers, sema inference, and HIR lowering exist in source but are not activated/committed. |
| **Spec only**     | No compiler implementation. |
| **Stub**          | CLI subcommand exists but returns "not implemented yet". |

---

## Compiler Pipeline

| Stage | Status | Notes |
|---|---|---|
| Lexer | **Working** | Hand-written, character-at-a-time. Longest-first maximal munch for all multi-char operators. `&&` and `||` are rejected with a dedicated error pointing to `and` / `or` |
| Parser | **Working** | Recursive-descent. Function decls, expressions, imports. |
| Formatter | **Working** | Round-trip stable for all 16 `ExprKind` nodes including `Index`, `OptionalProp`, `Field`, `Arrow`, and `StructLiteral` |
| Import resolution | **Working** | `import`, `from`, `export`, `alias`, `type` |
| Name resolution | **Working** | Scope-chained `lookupBinding`. Per-scope `DuplicateDecl`. |
| Type checking | **Working** | All `ExprKind` nodes. Optional/null validation. Index bounds. |
| Generic instantiation | Partial | Basic `<T>` works; `T: Trait` constraints not enforced |
| Comptime / Solve | Partial | Present in the documented target pipeline before ownership proof; some current frontend lowering still needs to stop erasing resource information before NRA |
| NTA / NRA | **In progress** | Pre-HIR residual-fact boundary is implemented: semantic facts are accumulated and consumed before final lowering; the alive/dead/lent state machine and full diagnostics remain |
| HIR lowering | **Working** | Covers all working features; stable boundary is `sema -> comptime/solve -> NTA/NRA -> HIR`, and residual ownership facts attach to side tables without introducing ownership HIR nodes |
| LLVM codegen | **Working** | x86-64 and WebAssembly targets |
| Cache | Partial | Object caching works; `.zirl` format not yet used |

---

## Language Features

### Functions & Bindings

| Feature | Status | Notes |
|---|---|---|
| `fn` | **Working** | Parameters, return type, body. Overloading by parameter count and types (F-33); linkage names are qualified as `<module>.<Owner>.<name>(<params>)`, except `extern fn` and `main` |
| `flow fn` | **Working** | Parsed and lowers; `marker`/`dock`/`jump` not exhaustively tested |
| `raw fn` | **Working** | Parsed and lowers |
| `const fn` | **Parse error** | `const` is a binding keyword; `const fn f()` parses as `const` binding named `fn`, not a const function |
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
| `*T` (pointer) | **Working** | Non-nullable: `null` requires `?*T`. `*p` deref, `&x` addr-of, and `->` arrow all work. `*void` is rejected (use `raw opaque`). Pointers imported from C are `?*T`, checked with `is null`; a `?*T` is still accepted unchecked where `*T` is expected |
| `raw opaque` | **Working** | Dedicated `TypeExprKind::Opaque`, lowered to pointer-to-void. Castable to and from any `*T` via `as`. Bare `opaque` (the tagged reference type) is still unimplemented and reports `unknown type` |
| `[N]T` (array), `[]T` (slice) | **Working** | Indexing on arrays, slices, and pointers lowers through HIR/LLVM |
| `dyn Trait` | **Parse error** | Type parser does not handle `dyn` |
| `struct`, `component`, `enum`, `union` | **Working** | Declarations parse and resolve |
| `trait`, `interface` | **Working** | Declarations parse and resolve |
| `implement T as Trait {}` | **Working** | Method bodies lowered |
| `type` alias | **Working** | |
| memory qualifiers (`mut`, `lend`, `view`, `unique`, `share`, `belong`) | **Working (parse + types)** | Accepted as type prefixes anywhere a type is written; carried in the type table as `TypeKind::Qualified`; writing through a `view` binding reports `E4004`. HIR/codegen strip the qualifier, and residual ownership facts are produced by the pre-HIR NTA/NRA boundary (F-34, partial F-14) |

### Expressions

| Feature | Status | Notes |
|---|---|---|
| literals (`42`, `0xFF`, `0c17`, `0b101`, `3.14`, `true`, `false`, `null`, strings, chars) | **Working** | Explicit radix prefixes (`0x` hex, `0c` octal, `0b` binary) are typed and lowered to their value; a literal wider than 64 bits reports E0004. Digit separators (`1_000`) are unsupported. C-like escapes decoded in string and char literals; unknown escapes report E0001 |
| unary `-`, `not` | **Working** | |
| unary `~` | **Working** | Bitwise NOT; integer operand only, lowers to `HirUnaryOp::BitNot` |
| binary `+` `-` `*` `/` `%` `==` `!=` `<` `>` `<=` `>=` | **Working** | |
| bitwise `&.` `|.` `^.` | **Working** | Spec spellings keep the `.`. Both operands must be integers of the same type; share `HirBinaryOp::And`/`Or`/`Xor` with the `and`/`or`/`xor` keywords |
| assignment `=` | **Working** | Right-associative, yields a value |
| compound assignment `+=` `-=` `*=` `/=` `%=` `<<=` `>>=` `&=` `|=` `^=` | **Working** | Desugared in the parser to `Assign(Binary(base))`, so they yield a value like `=` and inherit its coercion and `view` checks. The bitwise compounds drop the `.` of their base spelling. No new HIR node |
| `&&`, `||` | **Parse error** | Lexed as single tokens purely to report a dedicated error pointing at `and` / `or`; exactly one diagnostic, no cascade |
| field access `x.field` | **Working** | Dot access on struct values |
| dereference `*p` | **Working** | Pointer dereference via unary `*` |
| address-of `&x` | **Working** | Address-of via unary `&` |
| `->` chain operator | **Working** | Arrow access on struct pointers (`p->field`) |
| index `a[i]` | **Working** | On arrays, slices, pointers; rejects non-indexable types |
| `?` postfix propagation | **Working** | Requires optional operand in optional-returning function |
| `as` cast | **Working** | Dedicated `ExprKind::Cast` -> `HirCast` -> LLVM conversion. Numeric pairs plus `raw opaque` <-> `*T` (`classifyCast`); pointer-to-pointer between concrete pointees, integer/pointer mixes and user-defined casts stay rejected. No narrowing overflow check |
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
| `when` pattern match | **In progress (uncommitted)** | `ExprKind::When` with `Range`/`Placeholder`/`LayoutIntrinsic`, `parseWhen`, `inferWhen`/`inferRange`, `lowerWhen`/`lowerWhenCondition` exist uncommitted. Arm syntax unrecognised. |
| `marker` / `jump` | **Working** | Block-style go-to: `marker` declares a labeled block, `jump` transfers control to it. `dock` not implemented |
| `dock` | **Parse error** | Not implemented yet |

### Words, Contexts, Macros

| Feature | Status | Notes |
|---|---|---|
| `prefix`, `suffix`, `infix`, `nop` decls | **Parse skipped** | Body skipped via `skipDelimited` |
| `context` declarations | **Parse skipped** | Body skipped |
| `use` statements | **Parse skipped** | Body skipped |
| `@macro` calls | **Working** | Normal macros rename template-local bindings hygienically and resolve other template names through the call-site scope (globals/imports visible when not shadowed). `raw macro` splices literally into the call-site scope and names resolve there before module/global fallback. Templates are not analysed as code; resolution is keyed by node id |
| `tag macro` calls | **Working** | `<Section ...> ... </Section>`; named attributes via `attributes.name`; statement-position only |
| word call expressions | **Parse error** | No parser support |
| word sequence expressions | **Parse error** | No parser support |

### Module System & Visibility

| Feature | Status | Notes |
|---|---|---|
| `import`, `from`, `export` | **Working** | Import resolution with correct paths |
| `alias` | **Working** | |
| `pub`, `mod` | **Working** | |
| `mod(..)`, `mod(N)` | Not verified | Parser accepts; sema behaviour unknown |
| C header imports | **Working (common C)** | libclang only; variadic functions, array-decayed parameters, `va_list`, and function-pointer parameters supported. Single unsupported decls are skipped and recorded in `skippedFunctions`; macros, globals, bitfields, packed/anonymous records and flexible arrays remain unimported. Struct-by-value ABI is not verified |

---

### Spec Only (No Compiler Support)

| Feature | Spec chapter |
|---|---|
| NRA ownership analysis (alive/dead/lent state machine; qualifiers themselves are implemented) | [07-memory-model.md](07-memory-model.md) |
| `comptime` evaluation | [11-comptime.md](11-comptime.md) |
| `const fn` evaluation | [11-comptime.md](11-comptime.md) |
| `fail` / `with` / `catch` / `must` / `throw` | [08-error-handling.md](08-error-handling.md) |
| Assets (`ZithProject.toml` asset paths) | [12-assets.md](12-assets.md) |
| `.zirl` binary format | [01-overview (§1.5)](Zith-spec.md) |
| `@appendField`, `@removeField`, `@appendMethod` | [11-comptime.md](11-comptime.md) |

| `match` | [09-control-flow.md](09-control-flow.md) |
| `dyn` dispatch | [14-polymorphism.md](14-polymorphism.md) |

---

## Legacy Reserved Syntax (Not Part of the Core Language Contract)

| Surface | Current behaviour | Notes |
|---|---|---|
| `async fn` | **Parse skipped** | Legacy parser affordance only. Concurrency is being documented as `stdlib`/runtime APIs, not a function kind |
| `yield` | Reserved token | Not a core statement |
| `spawn`, `await` | Reserved tokens | Not core operators or statements; no frontend/HIR contract depends on them |

---

## CLI Commands

| Command | Status | Notes |
|---|---|---|
| `zithc build` | **Working** | Links an executable into `target/` by default; `--emit obj/ir/asm/hir` stop earlier |
| `zithc run` | **Working** | Compiles + executes in one step; the program's stdout/stderr is forwarded to zithc's **stdout**, compiler diagnostics stay on stderr |
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
All statuses above were verified against source at commit `bf7925e` (direct code inspection, no build):

`when`/`Range`/`Placeholder`/`LayoutIntrinsic` AST nodes and their sema/HIR handlers
exist in source but are not yet committed/activated.

---

## Known Debt

Recorded deliberately; each item is a follow-up, not an unknown.

| Item | Notes |
|---|---|
| Formatter re-prints `for (cond)` as `while` | `for` reuses `ExprKind::While`; a distinct node is needed to round-trip the spelling |
| No overflow check on narrowing conversions | Neither `as` nor numeric-literal adaptation validates that the value fits the target |
| Unchecked `?*T` -> `*T` coercion | Every C pointer is `?*T`, but without flow-sensitive narrowing it is accepted unchecked where `*T` is expected. Isolated in `PerModuleSema::allowsUncheckedNullablePointer`; delete it when narrowing lands |
| No flow-sensitive narrowing after `is null` | `p->field` on a `?*T` requires NonNull proof from `if (p is null) { } else { p->field }` or `for (not (p is null))`. Error code `E3005` |
| `is` limited to `is null` | Union/type narrowing is not addressed |
| `for` iterator and 3-clause forms unimplemented | Reported as errors rather than parsed |
| User-defined casts | To be added as a new branch in `classifyCast` |
| No C struct-by-value ABI | `struct` parameters/results import as named foreign types, but there is no verified ABI and no Zith-visible layout, so constructing/passing records to C remains unsupported |
| `..` lexes per character | Its `precedence()` is -1 and the when-case range pattern depends on the two `.` tokens. Every other multi-char operator is munched longest-first as one token and wired through the parser, sema and formatter |
| `++` / `--` | Not implemented; no increment/decrement operators exist |
| Ownership proof still happens after premature lowering in places | The stable order is `sema -> comptime/solve -> NTA/NRA -> HIR`; residual facts are now attached before final lowering, while some paths still need the full NRA proof before emitting their final form |

*When a feature moves from one status to another, update this table and re-verify.*
