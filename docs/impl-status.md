# Zith Implementation Status

> Last updated: 2026-08-28.

This document is the single source of truth for what the compiler supports today. Status reflects
actual compiler behaviour at baseline `a5f3716`. Each feature was verified by running
`build/zithc check` against a standalone test file, with source inspection where a status depends
on internal structure; status reflects actual compiler behaviour, not spec intent.

---

## Status Legend

| Label | Meaning |
|---|---|
| **Working**       | Accepted by parser and sema. Lowers through HIR to LLVM codegen. |
| **Check only**    | Passes `zithc check` but semantics are incidental (parsed as Name / Binary). No dedicated AST node, HIR, or codegen. |
| **Parse skipped** | Declaration accepted; body entirely skipped by `skipDelimited('{', '}')`. No semantics. |
| **Parse error**   | The parser itself rejects this construct. Does not reach sema. |
| **Parse-level in progress** | Parser lowers the construct into a frontend declaration, but the feature's semantics or full lowering are not complete. |
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
| Generic instantiation | **Working (step 04/05)** | Generic `fn`, `struct`, `alias`, `enum`, `union`, and `implement` blocks are monomorphized before HIR. Calls and named types resolve concrete instances. `T: A + B` bounds are parsed, stored, and enforced at generic call sites; trait-bound method calls type-check through the declared trait method |
| Comptime / Solve | **Reserved** | Macro expansion happens in frontend; the solver remains a compatibility stub. Generic monomorphization now runs before NRA/HIR in step-04 |
| NRA / Reference Analysis | **In progress** | NRA is the full Zith reference/ownership analysis. Zith-- implements a partial simplified version: residual facts are accumulated and consumed before final lowering, while the full alive/dead/lent state machine and four-rule proof remain to be completed. Internal names such as `NraFacts` and `nraStage` keep the historical NRA spelling |
| HIR lowering | **Working** | Covers all working features; residual ownership facts attach to side tables without introducing ownership HIR nodes |
| LLVM codegen | **Working** | x86-64 and WebAssembly targets |
| Cache | Partial | Object caching works; `.zirl` format not yet used |

---

## Language Features

### Functions & Bindings

| Feature | Status | Notes |
|---|---|---|
| `fn` | **Working** | Parameters, return type, body. The return type is written `fn f(x: T): R` or `fn f(x: T) -> R`; both spellings parse. Overloading by parameter count and types (F-33); linkage names are qualified as `<module>.<Owner>.<name>(<params>)`, except `extern fn` and `main` |
| generic parameter lists `<T, U>` | **Working (step 04/05)** | Accepted on `fn`, `struct`, `type` alias, `enum`, `union` and `trait` declarations. Generic calls and concrete type uses instantiate monomorphically, including inferred type arguments and generic methods. Cache artifacts carry an instantiation summary. `T: A + B` bounds are parsed and enforced with `E3009` (trait) or `E2024` (interface) when an argument does not satisfy a bound |
| `state` machine | **Working** | `state` declarations, `dock State(args)` expressions, and `jump Next(args)` terminating transitions are parsed, typed, and lowered. States in one machine share a return type but may have different parameter lists; a state without a written return type is `void` and is not inferred from the body. State functions and calls use LLVM `tailcc`; transitions emit direct `musttail tailcc` calls followed by `ret`; old `flow fn`/`marker` syntax is rejected |
| `raw fn` | **Working** | Parsed and lowers |
| `const fn` | **Parse-level in progress** | Parsed as a function declaration with `FunctionKind::Const`; compile-time evaluation is not implemented |
| `extern fn` | **Working** | C ABI interop |
| `let`, `var`, `const`, `global` | **Working** | All binding forms. `const` means immutable, not comptime |

### Types

| Feature | Status | Notes |
|---|---|---|
| `bool`, `char` | **Working** | |
| `i8`–`i128`, `u8`–`u128` | **Working** | Arithmetic between matching widths only; no implicit promotion |
| `f32`, `f64` | **Working** | Same-width arithmetic only |
| `?T` (optional) | **Working** | `null → ?T` and `T → ?T` coercions; `?` postfix propagation with operand/return validation. In boolean conditions, `x?` on `?T` means `x != null`. `null` is rejected for non-optional `*T` |
| `T!` (failable) | **Working** | Declared type; lowered through HIR |
| `*T` (pointer) | **Working** | Non-nullable: `null` requires `?*T`. `*p` deref, `&x` addr-of, and `->` arrow all work. `*void` is rejected (use `raw opaque`). Pointers imported from C are `?*T`, checked with `is null`; a `?*T` is still accepted unchecked where `*T` is expected |
| `raw opaque` | **Working** | Dedicated `TypeExprKind::Opaque`, lowered to pointer-to-void (untagged C-style `void*`). Castable to and from any `*T` via `as`; `raw opaque as T` reinterprets without a tag check |
| `opaque` | **Working** | Bare opaque is a tagged open union stored as `{ *void, u32 }` (typeId) and is always a view. `T as opaque` spills the concrete value to a stable local; `opaque is T` compares the module-local typeId; `opaque as T` returns `?T` (extraction in a call or other non-optional result is handled as a checked optional). This iteration has no heap copy, vtable or dynamic calls, and bare `opaque` typeIds are module-local only |
| `[N]T` (array), `[]T` (slice) | **Working** | Arrays coerce to slices as zero-copy views; `a[lo..hi]` and `a[i]` return optionals with static/dynamic bounds checks. `raw a[lo..hi]`/`raw a[i]` emit unchecked views/indexing |
| `[...]T` (variadic slice) | **Working** | Last parameter only. Auto-collects a homogeneous tail into a temporary slice; accepts an explicit final `[]T`/`[N]T` and an empty tail. Supported for free functions, methods, dyn trait/interface methods, generic inference, states/dock/jump and overloads; fixed-arity overloads are preferred |
| `fn(...): R` (function value) | **Working** | Parses as a type value, type-checks non-generic function references, and lowers/calls through C function-pointer ABI. No closures or captures |
| `dyn Trait` / `dyn Interface` | **Working (methods)** | Concrete values coerce to fat pointers (`HirMakeDyn`) with per-type vtables (`HirVTable`); method calls lower to `HirDynCall`. Zith-- exposes only methods through `dyn`; interface fields remain available on concrete types and generic bounds, and `a.x` on `dyn Interface` reports `E3001` |
| `struct`, `component`, `enum`, `union` | **Working** | Declarations parse and resolve |
| `trait`, `interface` | **Working (conformance)** | Declaration bodies store trait method requirements/default methods and interface fields plus declaration-only method requirements. Single (`x: T`) and grouped (`[x, y]: T`) interface fields are equivalent. Trait implementations are verified against required signatures with `Self` substitution; duplicate implementations are rejected (`E2027`). Interfaces satisfy structurally by checking fields and compatible method signatures, generic interface bounds expose those members, and explicit interface implementation is rejected (`E2025`) |
| `implement T as Trait {}` | **Working** | Records a verified nominal conformance edge and resolves calls to concrete trait defaults. The canonical syntax is `implement T as Trait`; the legacy `for Trait` spelling remains parsed |
| `type` | **Partial** | `type Name = T` creates a nominal one-field wrapper and is not interchangeable with `T`; construction/field access still need an explicit value syntax |
| `alias` | **Working** | Transparent alias: `alias Name = T` re-exports the same type |
| memory qualifiers (`mut`, `lend`, `view`, `unique`, `share`, `belong`) | **Working (lend/view slice)** | `lend T`/`view T` parameters lower to pointers and require call-site annotations for `default` bindings (`E4005`); invalid call annotations are rejected (`E4007`); same-binding conflicts in one call are rejected; `view` writes report `E4004`; LLVM adds `readonly` for `view` and `nocapture` for `lend`/`view`. `unique`/`share`/`belong`/`mut` as type prefixes remain rejected or legacy-only. NRA residual facts are attached before HIR (F-34, partial F-14) |

### Expressions

| Feature | Status | Notes |
|---|---|---|
| literals (`42`, `0xFF`, `0c17`, `0b101`, `3.14`, `true`, `false`, `null`, strings, chars) | **Working** | Explicit radix prefixes (`0x` hex, `0c` octal, `0b` binary) are typed and lowered to their value; a literal wider than 64 bits reports E0004. Digit separators (`1_000`) are unsupported. C-like escapes decoded in string and char literals; `\#` is an accepted escape producing a literal `#`; unknown escapes report E0001 |
| unary `-`, `not` | **Working** | `not` is the only boolean negation; prefix `!` is not recognized and stays reserved for a future postfix form |
| unary `~` | **Working** | Bitwise NOT; integer operand only, lowers to `HirUnaryOp::BitNot` |
| binary `+` `-` `*` `/` `%` `==` `!=` `<` `>` `<=` `>=` | **Working** | |
| bitwise `&.` `|.` `^.` | **Working** | Spec spellings keep the `.`. Both operands must be integers of the same type; share `HirBinaryOp::And`/`Or`/`Xor` with the `and`/`or`/`xor` keywords |
| assignment `=` | **Working** | Right-associative, yields a value |
| compound assignment `+=` `-=` `*=` `/=` `%=` `<<=` `>>=` `&=` `|=` `^=` | **Working** | Desugared in the parser to `Assign(Binary(base))`, so they yield a value like `=` and inherit its coercion and `view` checks. The bitwise compounds drop the `.` of their base spelling. No new HIR node |
| `&&`, `||` | **Parse error** | Lexed as single tokens purely to report a dedicated error pointing at `and` / `or`; exactly one diagnostic, no cascade |
| field access `x.field` | **Working** | Dot access on struct values. Struct fields are private by default; `pub name: T` opens a field, and `mod`/`mod(N)`/`mod(..)` apply the existing module-depth rule. Invisible fields are rejected for access and in cross-module struct literals |
| dereference `*p` | **Working** | Pointer dereference via unary `*` |
| address-of `&x` | **Working** | Address-of via unary `&` |
| `->` chain operator | **Working** | Arrow access on struct pointers (`p->field`) |
| index `a[i]` | **Working** | On arrays, slices, pointers; array/slice reads return `?T` with bounds checks. `raw a[i]` skips bounds handling and returns `T` |
| `?` postfix propagation | **Working** | Requires optional operand in optional-returning function. In `if`/`while`/`for (cond)`, `x?` on `?T` is accepted as a non-null boolean test without requiring an optional return |
| `as` cast | **Working** | Dedicated `ExprKind::Cast` -> `HirCast` -> LLVM conversion. Numeric pairs plus `raw opaque` <-> `*T` (`classifyCast`); pointer-to-pointer between concrete pointees, integer/pointer mixes and user-defined casts stay rejected. Tagged-union member extraction outside a narrowed/checked context requires `raw`; raw-union member casts remain free. No numeric narrowing overflow check |
| `is null` | **Working** | Dedicated `ExprKind::IsNull`. Requires an optional operand; `?*T` uses the nullptr niche, `?T` reads the discriminant |
| `is <type>` | **Working (tagged unions + opaque)** | Tagged-union member tests lower to a runtime tag check; inside `if`/`when` they narrow the tested local to the member type. `opaque is T` compares the bare opaque typeId and returns `bool` |
| range `1..5` | **Check only** | Parsed as binary `..`; no dedicated sema |
| struct literal `Foo { x: 1, y: 2 }` | **Working** | Struct literal with named fields via `{}` syntax. Inaccessible private/mod fields are rejected except in the file that declares the struct |
| `@sizeOf`, `@offsetOf`, `@alignOf` | **Working** | `@` parses in expression position. `@sizeOf(T)` accepts any complete type and types as `u64`; `@offsetOf(S, field)` and `@alignOf(S)` are struct-only and type as `i32`. `@sizeOf(void)` reports `E3001` ("requires a complete type") |

### Control Flow

| Feature | Status | Notes |
|---|---|---|
| `if` / `else` / `else if` | **Working** | Conditions accept `bool` and, in condition position, `x?` on `?T` as a non-null test |
| `while` | **Deprecated** | Still lowers correctly, emits `W1008` suggesting `for (cond) { }`, and accepts `x?` as a non-null condition |
| `break`, `continue` | **Working** | Unlabeled forms target the innermost active loop. Labels accept `outer: for`, `break outer;`, and `continue outer;`; unknown or duplicate active labels are rejected |
| `return` (void and typed) | **Working** | |
| `for (cond) { }`, `for { }` | **Working** | Conditional and infinite loop forms lower to the same CFG as `while`; labels are supported on both forms. `x?` is accepted in condition position as a non-null test |
| `for (init, cond, step) { }` | **Working** | Flat and parenthesized clause forms are accepted. `init` and `step` are both optional; `continue` still runs the step before the next test. Labels are stored on the real `For` node |
| `for (x in xs)` | **Working** | Duck-typed iterator over a struct with `next(self)`; `next` returns a tagged union containing the element and the empty `End` marker, and the loop exits when the returned member is `End`. Labels are supported |
| `when` / `match` pattern match | **Working** | Arms are written `(pattern) ~> body`, comma-separated; `match` is a parser synonym for `when`. Equality, boolean and range (`1..3`) patterns lower through HIR to codegen. An `(f is Member)` arm narrows `f` for that arm's body. `(_)` is the default arm and must come last; a value-producing `when` without a default reports non-exhaustive. Covered by runtime tests |
| `state` / `jump` | **Working** | `state Name(params): ReturnType` declares a state with the machine return type; `jump Next(args)` terminates the current block and validates arity/types against the target's own parameters before a direct `musttail tailcc` transfer |
| `dock` | **Working** | `dock State(args)` is a `tailcc` call expression that returns the machine's final `state` return value; the old `dock { ... }` block form is rejected |
| `defer expr;` scope guards | **Working** | `defer expr;` and `defer { ... }` register cleanup on the nearest lexical block and run in reverse registration order on normal exit, `return`, `break`, `continue`, and `state` `jump`. A defer may capture same-block bindings declared later; exits before initializing a captured binding are rejected. The deferred body is cleanup-only and rejects `return`/`break`/`continue`/`jump` |
| `drop` cleanup hooks | **Spec only** | Reserved keyword only, no parser branch consumes it. Candidate for the next iteration (F-41) after `defer` |

### Words, Contexts, Macros

| Feature | Status | Notes |
|---|---|---|
| `prefix`, `suffix`, `infix`, `nop` decls | **Parse skipped** | Body skipped via `skipDelimited` |
| `context` declarations | **Parse skipped** | Body skipped |
| `use` statements | **Parse skipped** | Body skipped |
| `macro` / `raw macro` declarations and `@name(...)` calls | **Working** | Normal macros rename template-local bindings hygienically and resolve other template names through the call-site scope (globals/imports visible when not shadowed). `raw macro` splices literally into the call-site scope and names resolve there before module/global fallback. Templates are not analysed as code; resolution is keyed by node id |
| `tag macro` calls | **Working** | `<Section ...> ... </Section>`; named attributes via `attributes.name`; statement-position only |
| word call expressions | **Parse error** | No parser support |
| word sequence expressions | **Parse error** | No parser support |

### Module System & Visibility

| Feature | Status | Notes |
|---|---|---|
| `import`, `from`, `export` | **Working** | Import resolution with correct paths |
| `alias` | **Working** | |
| `pub`, `mod` | **Working** | |
| `mod(..)`, `mod(N)` | **Working** | Module-depth visibility is applied to declarations and to struct fields; `mod(..)` is unlimited and `mod(N)` allows N directory levels below the owner |
| C header imports | **Working (common C)** | libclang only; variadic functions, array-decayed parameters, `va_list`, and function-pointer parameters supported. Object-like scalar macros are imported as constants. Single unsupported decls/macros are skipped and recorded in `skippedFunctions`; function-like macros, strings, globals, bitfields, packed/anonymous records and flexible arrays remain unimported. Struct-by-value ABI is not verified |

---

### Spec Only (No Compiler Support)

| Feature | Spec chapter |
|---|---|
| NRA ownership analysis (full alive/dead/lent state machine and four-rule proof; the call-annotation borrow slice is implemented) | [07-memory-model.md](07-memory-model.md) |
| `comptime` evaluation | [11-comptime.md](11-comptime.md) |
| `const fn` evaluation | [11-comptime.md](11-comptime.md) |
| `fail` / `with` / `catch` / `must` / `throw` | [08-error-handling.md](08-error-handling.md) |
| Assets (`ZithProject.toml` asset paths) | [12-assets.md](12-assets.md) |
| `.zirl` binary format | [01-overview (§1.5)](Zith-spec.md) |
| `@appendField`, `@removeField`, `@appendMethod` | [11-comptime.md](11-comptime.md) |
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
| `zithc build` | **Working** | Links an executable into `target/` by default; `--emit obj/ir/asm/hir` stop earlier; `--cache-stats` prints object-cache hit/miss counts |
| `zithc run` | **Working** | Compiles + executes in one step; the program's stdout/stderr is forwarded to zithc's **stdout**, compiler diagnostics stay on stderr |
| `zithc check` | **Working** | Type-checks without emitting. Errors forwarded from frontend snapshot |
| `zithc fmt` | **Working** | Round-trip tested for `Index` and `OptionalProp` |
| `zithc create <name>` | **Working** | |
| `zithc clean` | **Working** | |
| `zithc execute <file>` | **Working** | |
| `zithc test <path>` | **Working** | Discovers and runs test files under the given path |
| `zithc repl` | **Stub** | |
| `zithc deps list` | **Working** | Reads `ZithProject.toml` and lists declared dependencies |
| `zithc deps add`, `deps remove` | **Stub** | |
| `zithc docs` | **Working** | Generates documentation from source |

---

## Diagnostic Codes

Codes are grouped by pipeline stage. `E0000` remains the generic user-reported diagnostic.

| Range | Stage | Codes |
|---|---|---|
| 0001-0005 | Lexical | `E0001` UnknownToken, `E0002` UnclosedString, `E0003` InvalidEscape, `E0004` InvalidIntLiteral, `E0005` UnclosedComment |
| 1001-1008 | Parse | `E1001` ExpectedExpr, `E1002` ExpectedSemicolon, `E1003` UnclosedParen, `E1004` ExpectedIdent, `E1005` InvalidImportDepth, `E1006` ImportError, `E1007` TopLevelLetNotAllowed, `W1008` DeprecatedSyntax (`while` -> `for (cond)`) |
| 2001-2010 | Semantic | `E2001` UndefinedIdent, `E2002` DuplicateDecl, `E2003` WrongArity, `E2004` UnusedDecl, `E2005` NotNamespace, `E2006` NoMember, `E2007` NoMatchingFn, `E2008` AmbiguousCall, `E2009` NotImplemented, `E2010` UnsupportedSyntax |
| 2021-2025 | Frontend/interface | Trait requirement/signature checks (`E2021`/`E2022`), `E2023` NotATrait, `E2024` InterfaceNotSatisfied, `E2025` explicit `implement` for an interface, `E2027` DuplicateImplementation |
| 3001-3009 | Types | `E3001` TypeMismatch, `E3002` CannotInfer, `E3003` InvalidCast, `E3004` CyclicType, `E3005` NullDerefUnproven, `E3006` CoercionFailure, `E3007` WidthMismatch, `E3008` OptionalViolation, `E3009` ConstraintNotSatisfied |
| 4001-4007 | NRA / ownership | `E4001` UseAfterMove (logical receiver move in sema), `E4002` BorrowConflict, `E4003` DoubleBorrow, `E4004` WriteThroughView, `E4005` OwnershipCoercionRequired, `E4007` InvalidCallOwnership — call annotations, borrow conflicts and views are checked in sema; `E4001` is emitted for post-method receiver use; `E4004` remains emitted for views |
| 5001-5002 | Lowering | `E5001` InvalidIR, `E5002` Unreachable |
| 10001-10004 | Runtime | `R10001` IndexOutOfBounds, `R10002` DivisionByZero, `R10003` NullDeref, `R10004` Panic |

---

## Verification
All statuses above were verified against the binary built from commit `a5f3716`, by running
`zithc check` on standalone files per feature and by inspecting the source where a status depends
on internal structure (pipeline boundaries, linkage naming, diagnostic ranges).

---

## Known Debt

Recorded deliberately; each item is a follow-up, not an unknown.

| Item | Notes |
|---|---|
| Formatter re-prints `for (cond)` as `while` | `for` reuses `ExprKind::While`; a distinct node is needed to round-trip the spelling |
| No overflow check on narrowing conversions | Neither `as` nor numeric-literal adaptation validates that the value fits the target |
| Unchecked `?*T` -> `*T` coercion | Every C pointer is `?*T`, but without flow-sensitive narrowing it is accepted unchecked where `*T` is expected. Isolated in `PerModuleSema::allowsUncheckedNullablePointer`; delete it when pointer narrowing after `is null` lands |
| No flow-sensitive narrowing after `is null` | `p->field` on a `?*T` requires NonNull proof from `if (p is null) { } else { p->field }` or `for (not (p is null))`. Error code `E3005` |
| `is` outside `null`/tagged-union contexts | Non-union `is Type` remains unsupported and reports a dedicated diagnostic |
| Ranges and range syntax in `for (x in 0..4)` | The iterator protocol supports user types with `next(self) -> union { T, End }`; literal range syntax is future work |
| User-defined casts | To be added as a new branch in `classifyCast` |
| No C struct-by-value ABI | `struct` parameters/results import as named foreign types, but there is no verified ABI and no Zith-visible layout, so constructing/passing records to C remains unsupported |
| Bare `opaque` is module-local | The deterministic typeId is stable for the same concrete type inside one module, but imported/cached opaque values are rejected with `E2010` because a cross-module registry is not implemented yet |
| `..` lexes per character | Its `precedence()` is -1 and the when-case range pattern depends on the two `.` tokens. Every other multi-char operator is munched longest-first as one token and wired through the parser, sema and formatter |
| `++` / `--` | Not implemented; no increment/decrement operators exist |
| Ownership proof still happens after premature lowering in places | The stable order is `sema -> comptime/solve -> NTA/NRA -> HIR`; residual facts are now attached before final lowering, while some paths still need the full NRA proof before emitting their final form |

*When a feature moves from one status to another, update this table and re-verify.*
