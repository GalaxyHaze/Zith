# Zith-- Implementation Status

> Last updated: 2026-10-28.

This document is the single source of truth for what the compiler supports in `Zith--` today.
`Zith--` is a sibling language to Zith, so these statuses do not describe Zith or imply that
`Zith--` is a partial stage of it. Status reflects actual compiler behaviour, not spec intent.
The complete feature inventory was last broadly
audited at baseline `a5f3716`; entries changed on 2026-10-26 were rechecked against the compiler
built from `7595e541`, with source inspection where status depends on internal structure. The test
command entries changed on 2026-10-27 were checked by native and VM v2 integration runs plus the
full CTest suite. Explicit-alignment behavior was checked by the native, VM v2, cache, ZIRL, and
full CTest suites on 2026-10-28.

Implementation work that is incomplete or needs review is tracked in
[implementation-debt.md](implementation-debt.md).

---

## Status Legend

| Label | Meaning |
|---|---|
| **Working**       | Accepted by parser and sema. Lowers through HIR to LLVM codegen. |
| **Recognized only** | Parser and sema recognize or validate the syntax, but it has no emitted or runtime behavior. |
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
| Lexer | **Working** | Hand-written, character-at-a-time. Longest-first maximal munch for all multi-char operators. `..` and `...` lex as one `Dots` token, distinguished by lexeme. `&&`, `||`, `++` and `--` are rejected with a dedicated error |
| Parser | **Working** | Recursive-descent. Function decls, expressions, imports. |
| Formatter | **Working** | Round-trip stable for all 18 `ExprKind` nodes including `Index`, `OptionalProp`, `Field`, `Arrow`, `StructLiteral`, `WhenGuard`, and `FunctionLiteral` |
| Import resolution | **Working** | `import path`, `import path as name`, `from path`, `export path`, and selectors; regular-file candidates precede same-named directory aggregation, platform variants are resolved as `foo.<arch>.<os>.zith`, `foo.<arch>.zith`, `foo.<os>.zith`, then generic `foo.zith`; qualified access works in expressions, types, constructors and methods. `export` injects the target's public symbols and creates one qualified namespace alias per exported prefix so shared-prefix facades fan out to every submodule |
| Name resolution | **Working** | Scope-chained `lookupBinding`. Per-scope `DuplicateDecl`. |
| Type checking | **Working** | All `ExprKind` nodes. Optional/null validation. Index bounds. |
| Generic instantiation | **Working** | Generic `fn`, `struct`, `alias`, `enum`, `union`, and `implement` blocks are monomorphized before HIR. Calls and named types resolve concrete instances; reified structs keep concrete type arguments as structural metadata, so nested generics such as `Entry<K, V>` inside `HashMap<K, V>` keep the correct slots, and generic `union` instances are named from their substituted members so a helper returning `Result<T, E>` yields the caller's concrete `Result<A, B>` (generic `Ok`/`Err` helpers over the positional union compile). Enum/union templates accept inline `fn` methods and generic `implement as Trait` blocks alongside their positional variants/members. Generic inference considers implicit optional coercions, so `?T`/`??T` parameters can infer `T` from a bare or partially-optional argument while non-optional parameters remain exact. Contextual (expected-result) inference binds parameters that appear only in the return type: a binding annotation, `return`, or `?*T` annotation unifies the declared result against the expected type, so `Ok(7)` and `new(3)` bind their result-only parameters without explicit `<...>`. An unsuffixed numeric literal binds such a parameter weakly, so a later strong binding (the expected result) overrides it. `T: A + B` bounds are parsed, stored, and enforced at generic call sites; trait-bound method calls type-check through the declared trait method while `GenericBinding` remains the source of truth for bounds |
| Comptime / Solve | **Reserved** | Macro expansion happens in frontend; the solver remains a compatibility stub. Generic monomorphization now runs before SRA/HIR in step-04 |
| SRA / Reference Analysis | **Working (frozen)** | SRA (Small Resource Analysis) is the ownership slice Zith-- implements and will not grow beyond: residual facts are accumulated and consumed before final lowering, and the `&local`/`@ptrOf(local)` use-after-move slice marks moved slots as consumed in `HirAttrs` without HIR ownership nodes. The full ownership proof belongs to Zith's separate design and is specified in [nra-spec.md](nra-spec.md); it is deliberately out of scope for Zith--. Internal names use the SRA spelling (`SraFacts`, `sraStage()`, `Stage::SraResolved`); the C ABI keeps `ZITHC_STAGE_NRA_RESOLVED` as a compatibility alias |
| HIR lowering | **Working** | Covers all working features; residual ownership facts attach to side tables without introducing ownership HIR nodes |
| LLVM codegen | **Working** | x86-64 and WebAssembly targets |
| Cache | **Working** | Object and artifact caching persist/load `.zirl` files, validate canonical mappings, and hydrate cached artifacts |
| Stdlib I/O | **Working** | `print`/`println`/`input`, `Formatable`, `ParseInput`, and `InputLine.cast<T>` type-check and lower; runtime parsing verification is manual for now |
| Stdlib allocation | **Working** | `std/memory` ships the strict DAG with `in-place`, `allocator`, `heap`, and `new`; imported `InPlace` conformance, facade-qualified `InPlace` and `HeapAllocator` types, and cache hydration are covered by `tests/test-interface-satisfaction.cpp` and `tests/test-generic-hashmap.cpp`. `std/alloc` remains as a legacy compatibility module. `stdlib/std/memory/new.zith` ships real `new<T>`/`delete<T>` bodies: `new` binds its result-only `T` from the expected result type, allocates via the `HeapAllocator` bridge, calls `(*p).inplace(args)`, and `delete` calls `(*p).clean()` and frees the block. The allocator-owned pair ships as free functions `make`/`release` over `dyn Allocator` in `allocators/allocator`; `test-codegen` runs both pairs (`test_imported_memory_new_delete_runtime`, `test_imported_memory_make_release_runtime`). The `Allocator.make`/`release` trait defaults remain const-failing until a trait default body can dispatch to another trait method on `self` |
| Stdlib collections | **Working** | `std/collections/hash_map_u64` ships a concrete `u64 -> u64` open-addressed map with `reserve`, `put`, `contains`, `get`, `len`, and `destroy`, consumable from other modules. `std/collections/hash_map` ships a checked generic `HashMap<K, V>`, `Entry<K, V>`, and `Hashable` module that is now consumable from another module: `hm.HashMap<i64, i64>{}` and `hm.HashMap.init(lend map)` reify from the declaring module's template, imported field defaults lower in the declaring module, and nested generic calls concretize transitively (`init<i64,i64>` calls `reserve<i64,i64>`). `examples/hash-map.zith` passes `zithc check` and runs to exit 0; `tests/test-generic-hashmap.cpp` and `test-codegen` cover the imported instance. `DynArray<T>` shares the path: `examples/dyn-array.zith` passes `zithc check` and runs to exit 0, and `test-codegen` exercises an imported `DynArray<i32>` end-to-end; see [imported generic instantiation](adr/0020-imported-generic-instantiation.md) |
| Stdlib error handling | **Working** | `std/result` ships the canonical positional `pub union Result<T, E> { T, E }` plus `pub fn Ok`/`Err`/`isOk`/`isErr`/`unwrap`/`unwrapErr` generic helpers. Result-only parameters infer from the expected result type (binding annotation or `return`), so `Ok(7)` binds `T`/`E` without an explicit `<...>`; see [stdlib I/O Result contract](../memory/stdlib-io-result.md). `std/panic` ships the fatal `panic(message)`, which writes the message and terminates through `R10004`; the native runtime panic helper flushes the C streams before trapping so the message survives the abort. `tests/test-stdlib-modules` type-checks both modules and `test-codegen` runs an imported `Result<T, E>` (`test_imported_result_runtime`) and `panic` (`test_imported_panic_runtime`) end-to-end. `Result<void, E>` remains a separate, larger item |

---

## Language Features

### Functions & Bindings

| Feature | Status | Notes |
|---|---|---|
| `fn` | **Working** | Parameters, return type, body. The return type is written `fn f(x: T): R` or `fn f(x: T) -> R`; both spellings parse. Overloading by parameter count and types (F-33); linkage names are qualified as `<module>.<Owner>.<name>(<params>)`, except `extern fn` and `main` |
| `[a, b]` binding destructuring | **Partial** | Pack values destructure positionally. A scalar type annotation broadcasts one initializer to all names, including direct pack literals and local scalar aliases. Direct pack annotations and unique local non-generic transparent pack aliases preserve positional extraction; imported, generic, and ambiguous pack aliases are not classified by the frontend yet |
| Call-result statements | **Working** | Non-void call/dock results used as expression statements must be consumed or explicitly discarded with `_ = expr;` (`E2026`). Void calls and value-producing uses (binding, assignment, return, argument, condition, binary operand, field/index operand, method dispatch) remain legal |
| `#[test]`, `@assert`, `@check`, `@log` | **Working** | `#[test]` marks a free `void` function with no parameters. `@assert(cond)` records its static failure id and returns from that test on false. `@check(cond)` emits a failure event and continues, so a test is failed once even if several checks fail. Both intrinsics panic outside the lexical body of a test; a macro expanded at the test call site inherits that context, but an ordinary helper function does not. The panic lowers to runtime code `R10004` on native and on VM v2 (the v2 lowering accepts any `R10001+` code, not only the optional-extraction `R10003`). `@log(msg)` writes framed output to stdout for the runner. The synthesized driver runs tests in declaration order and prints `zith-test <name> <slot>`, where the slot is `0` on success or the failing assert id; check failures use separate events. Assert/check locations are encoded in the ZIRL `Debug` section. `#[test]` with parameters or a non-void return reports `W2029`; a module declaring both `main` and `#[test]` reports `E2010`. `std/test` provides accumulating `check*` and fail-fast `assert*` helpers with formatted messages on native and VM v2; rich formatting still has an HIR interpreter gap. |
| generic parameter lists `<T, U>` | **Working (step 04/05)** | Accepted on `fn`, `struct`, `type` alias, `enum`, `union` and `trait` declarations. Generic calls and concrete type uses instantiate monomorphically, including inferred type arguments and generic methods. Cache artifacts carry an instantiation summary. `T: A + B` bounds are parsed and enforced with `E3009` (trait) or `E2024` (interface) when an argument does not satisfy a bound |
| `state` machine | **Working** | `state` declarations, `dock State(args)` expressions, and `jump Next(args)` terminating transitions are parsed, typed, and lowered. States in one machine share a return type but may have different parameter lists; a state without a written return type is `void` and is not inferred from the body. State functions and calls use LLVM `tailcc`; transitions emit direct `musttail tailcc` calls followed by `ret`; old `flow fn`/`marker` syntax is rejected. `state(params): ret` is a value type that accepts a real matching `state` declaration and supports `dock S(args)` indirect calls |
| Function return termination | **Working** | Non-void functions reject paths that can fall through without a value. Implicit final-value bodies, complete `if`/`else` or `when` with a default, state `jump`, and unbounded `for` loops without a direct `break` are accepted; otherwise the body reports a missing-return diagnostic |
| Inferred function return type | **Working** | Unannotated functions infer from their tail expression and explicit `return` statements. Bodies without a value infer `void`. Direct and indirect call cycles are rejected with `E3002`, and incompatible return values fail sema. `union` and `dyn` results still require an explicit annotation |
| `raw fn` | **Working** | Parsed and lowers |
| `const fn` | **Parse error** | Rejected by the parser; compile-time evaluation is not part of Zith-- |
| `extern fn` | **Working** | C ABI interop |
| `let`, `var`, `const`, `global` | **Working** | All binding forms. `const` means immutable, not comptime |
| `#[alignas(N)]` on bindings | **Working** | Applies to supported `let`, `var`, or `const` storage. `N` is a positive power-of-two constant supported by the target. It aligns only that binding, not its type, copies, or individual array elements. |
| `#[noalias]`, `#[likely]`, `#[unlikely]` | **Recognized only** | `noalias` is restricted to functions returning pointers. `likely` and `unlikely` do not promise optimizer behavior. |

### Types

| Feature | Status | Notes |
|---|---|---|
| `bool`, `char` | **Working** | |
| `i8`–`i128`, `u8`–`u128` | **Working** | Arithmetic between matching widths only; no implicit promotion |
| `f32`, `f64` | **Working** | Same-width arithmetic only |
| `?T` (legacy optional wrapper) | **Working** | Zith-- compatibility support for `null → ?T` and `T → ?T` coercions, including nested `??T` accepting `?T` or bare `T`; `?` postfix propagation with operand/return validation. Generic inference uses the optional coercion as a fallback, so `fn wrap<T>(x: ?T): ?T` accepts `wrap(3)` and infers `T = i32`. Any `?T` expression is implicitly boolean in conditions (`if (x)` means `x != null`). Full-Zith use of `?T` as a type wrapper is deprecated. `null` is rejected for non-optional `*T` |
| `T!` wrapper / return annotation | **Spec only** | The wrapper form is deprecated. In full Zith, `fn f(): T!` declares success type `T` and lets the compiler infer invalid states; this annotation is not part of the Zith-- type surface |
| `*T` (pointer) | **Working** | Non-nullable pointer object: `null` requires `?*T`. `*p` deref, `&x` addr-of, and `->` arrow all work. `&x` is a logical move (`E4001` on later reads, direct rebind revives) and `&x`/`@ptrOf(local)` may not escape their storage scope (`E4008`). `*void` is rejected (use `raw opaque`). Pointers imported from C are `?*T`, narrowed by `is null`/`not (is null)`; deref, arrow, index, and coercions expect proof and report `E3005` otherwise |
| `raw opaque` | **Working** | Dedicated `TypeExprKind::Opaque`, lowered to pointer-to-void (untagged C-style `void*`). Castable to and from any `*T` via `as`; `raw opaque as T` reinterprets without a tag check |
| `opaque` | **Working** | Bare opaque is a tagged open union stored as `{ *void, u32 }` (typeId) and is always a view. `T as opaque` spills the concrete value to a stable local; `opaque is T` compares the tagged typeId; `opaque as T` returns `?T` (extraction in a call or other non-optional result is handled as a checked optional). Values exported and imported across modules retain the declaring module's canonical tag, and cached artifacts hydrate `canonical_mappings` so the tag stays stable across sessions. This iteration has no heap copy, vtable or dynamic calls |
| `[N]T` (array), `[]T` (slice) | **Working** | Arrays coerce to slices as zero-copy views; `a[lo..hi]` and `a[i]` return optionals with static/dynamic bounds checks. `raw a[lo..hi]`/`raw a[i]` emit unchecked views/indexing |
| `|T, U|` (pack) | **Working** | Heterogeneous pack literals, named pack types, positional compile-time indexing, and positional `[a, b]` destructuring lower as concrete aggregates. Packs can coerce positionally to named pack types with matching arity and member types |
| `[...]T` (variadic slice) | **Working** | Last parameter only. Auto-collects a homogeneous tail into a temporary slice; accepts an explicit final `[]T`/`[N]T` and an empty tail. Supported for free functions, methods, dyn trait/interface methods, generic inference, states/dock/jump and overloads; fixed-arity overloads are preferred |
| `fn(...): R` (function value) | **Working** | Parses as a type value, type-checks non-generic function references, and lowers/calls through C function-pointer ABI. No closures or captures |
| `fn (params): R { body }` (function literal) | **Working** | Anonymous function written in expression position, with no capture list. Parses as `ExprKind::FunctionLiteral`, types as a `FunctionType`, and lowers to a synthesized local function whose address is taken, so the value is a plain function pointer passed and called through the existing indirect-call path. A reference to an enclosing local inside the body is rejected as an unknown identifier (`E2001`), never captured. See [ADR-0051](adr/0051-anonymous-functions-in-zith-minusminus.md) |
| `dyn Trait` / `dyn Interface` | **Working (methods)** | Concrete values coerce to fat pointers (`HirMakeDyn`) with per-type vtables (`HirVTable`); method calls lower to `HirDynCall`. Zith-- exposes only methods through `dyn`; interface fields remain available on concrete types and generic bounds, and `a.x` on `dyn Interface` reports `E3001` |
| `struct`, `component`, `enum`, `union` | **Working** | Declarations parse and resolve. Ordinary structs support `#[alignas(N)]`, which affects field offsets, aggregate size, containing layouts, and array stride. Unverified aligned records are rejected at C by-value boundaries. Generic `enum`/`union` templates support inline methods, concrete instantiation for HIR/codegen, and trait conformance (`implement Enum<T> as Trait` / `implement Union<T> as Trait`) |
| `trait`, `interface` | **Working (conformance)** | Declaration bodies store trait method requirements/default methods and interface fields plus declaration-only method requirements. Single (`x: T`) and grouped (`[x, y]: T`) interface fields are equivalent. Trait implementations are verified against required signatures with `Self` substitution; duplicate implementations are rejected (`E2027`). Interfaces satisfy structurally by checking fields and compatible method signatures, generic interface bounds expose those members, and explicit interface implementation is rejected (`E2025`) |
| `implement T as Trait {}` | **Working** | Records a verified nominal conformance edge and resolves calls to concrete trait defaults. The canonical syntax is `implement T as Trait`; the legacy `for Trait` spelling remains parsed |
| `type` | **Working (Zith-- cast-based)** | `type Name = T` creates a nominal one-field wrapper distinct from `T`; `T as Name` constructs it and `Name as T` extracts the underlying value. `alias` stays transparent |
| `alias` | **Working** | Transparent alias: `alias Name = T` re-exports the same type |
| memory qualifiers (`mut`, `lend`, `view`, `own`, `share`, `belong`) | **Working (lend/view slice)** | `lend T`/`view T` parameters lower to pointers and require call-site annotations for `default` bindings (`E4005`); invalid call annotations are rejected (`E4007`); same-binding conflicts in one call are rejected; `view` writes report `E4004`; LLVM adds `readonly` for `view` and `nocapture` for `lend`/`view`. `own`/`share`/`belong`/`mut` as type prefixes remain rejected or legacy-only. NRA residual facts are attached before HIR (F-34, partial F-14) |

### Expressions

| Feature | Status | Notes |
|---|---|---|
| literals (`42`, `0xFF`, `0c17`, `0b101`, `3.14`, `true`, `false`, `null`, strings, chars) | **Working** | Explicit radix prefixes (`0x` hex, `0c` octal, `0b` binary) are typed and lowered to their value; a literal wider than 64 bits reports E0004. Digit separators (`1_000`) are unsupported. C-like escapes decoded in string and char literals, including octal `\NNN` (one to three digits; `\033` is ESC); `\#` is an accepted escape producing a literal `#`; a char literal must decode to exactly one character; unknown escapes report E0003 during type checking |
| unary `-`, `not` | **Working** | `not` is the only boolean negation; prefix `!` is not recognized and stays reserved for a future postfix form |
| unary `~` | **Working** | Bitwise NOT; integer operand only, lowers to `HirUnaryOp::BitNot` |
| binary `+` `-` `*` `/` `%` `==` `!=` `<` `>` `<=` `>=` | **Working** | |
| bitwise `&.` `|.` `^.` | **Working** | Spec spellings keep the `.`. Both operands must be integers of the same type; share `HirBinaryOp::And`/`Or`/`Xor` with the `and`/`or`/`xor` keywords |
| assignment `=` | **Working** | Right-associative, yields a value |
| compound assignment `+=` `-=` `*=` `/=` `%=` `<<=` `>>=` `&=` `|=` `^=` | **Working** | Desugared in the parser to `Assign(Binary(base))`, so they yield a value like `=` and inherit its coercion and `view` checks. The bitwise compounds drop the `.` of their base spelling. No new HIR node |
| `&&`, `||` | **Parse error** | Lexed as single tokens purely to report a dedicated error pointing at `and` / `or`; exactly one diagnostic, no cascade |
| `++`, `--` | **Parse error** | Lexed as single tokens purely to reject increment/decrement; the diagnostic points to explicit assignment. Prefix and postfix forms both report `E2010` |
| field access `x.field` | **Working** | Dot access on struct values. Struct fields are private by default; `pub name: T` opens a field, and `mod`/`mod(N)`/`mod(..)` apply the existing module-depth rule. Invisible fields are rejected for access and in cross-module struct literals |
| dereference `*p` | **Working** | Pointer dereference via unary `*` |
| address-of `&x` | **Working** | Address-of via unary `&` |
| `p->field` | **Working** | Arrow access on struct pointers |
| `|>` / `do` pipeline | **Working** | `|>` requires exactly one explicit `..`; `do` accepts zero or one `..`, keeps the chain value and may run effects without it. No automatic injection or `?`/`!`/tag propagation; `|>` replaces the chain value |
| index `a[i]` | **Working** | On arrays, slices, pointers; array/slice reads return `?T` with bounds checks. `raw a[i]` skips bounds handling and returns `T` |
| `?` postfix propagation | **Working** | Requires optional operand in an optional-returning function. `?T` conditions are implicit (`if (x)`), so `x?` is propagation only and is rejected in condition position unless the enclosing function propagates |
| `must` / `raw` optional extraction | **Working** | `must x` extracts a `?T` payload and terminates with runtime panic `R10003` on `null`; `raw x` extracts the payload without a null check. `is null` remains the only flow-narrowing mechanism |
| `as` cast | **Working** | Dedicated `ExprKind::Cast` -> `HirCast` -> LLVM conversion. Numeric pairs plus `raw opaque` <-> `*T` (`classifyCast`); `null as ?T` fixes the optional type of a bare `null`. Pointer-to-pointer between concrete pointees, integer/pointer mixes and user-defined casts stay rejected. Tagged-union member extraction outside a narrowed/checked context requires `raw`; raw-union member casts remain free. Narrowing literal, `-` literal and integer-const operands are checked at compile time; variable operands and float-to-int still lack runtime checks |
| `is null` | **Working** | Dedicated `ExprKind::IsNull`. Requires an optional operand; `?*T` uses the nullptr niche, `?T` reads the discriminant. The early-return guard `if (x is null) { return ...; }` narrows both `?*T` and optional aggregates (`?S`, `?pack`, `?i32`), so `x.field`, `x.len`, and `return x;` read the payload after the guard |
| `is <type>` | **Working (tagged unions + opaque)** | Tagged-union member tests lower to a runtime tag check; inside `if`/`when` they narrow the tested local to the member type. `opaque is T` compares the bare opaque typeId and returns `bool` |
| range `1..5`, `1>..5`, `1..<5`, `1>..<5` | **Working** | Dedicated `ExprKind::Range` with raw bounds and open-at-lo/hi flags. `in` lowers ranges and any `contains(self, value): bool` method; `for (x in int_range)` iterates with implicit step `1`. Float ranges are valid with `in` and rejected in `for`. Boundary semantics are documented and covered, including empty ranges and single-value closed ranges |
| `in` (`value in rhs`) | **Working** | Binary operator returning `bool`; resolves literal ranges and the `Contains` protocol without changing the existing `Iterator` protocol |
| struct literal `Foo { x: 1, y: 2 }` | **Working** | Struct literal with named fields via `{}` syntax. Inaccessible private/mod fields are rejected except in the file that declares the struct |
| `@sizeOf`, `@offsetOf`, `@alignOf` | **Working** | `@` parses in expression position. `@sizeOf(T)` accepts any complete type and types as `u64`; `@alignOf(T)` accepts any complete type (including primitives and generic `T`) and types as `i32`; `@offsetOf(S, field)` is struct-only and types as `i32`. `@sizeOf(void)`/`@alignOf(void)` report `E3001` ("requires a complete type") |
| `@canonicalType` | **Working** | Returns a stable `u128` derived from module namespace, canonical field order, and type name; lowers to `HirCanonicalType` and is serialized through the artifact cache. |

### Control Flow

| Feature | Status | Notes |
|---|---|---|
| `if` / `else` / `else (cond)` / `else if` | **Working** | Conditions accept `bool` or `?T`; `?T` is tested implicitly as non-null. `else (cond)` is the preferred chained spelling; `else if` is deprecated with `W1008` but still lowers |
| `while` | **Deprecated** | Still lowers correctly, emits `W1008` suggesting `for (cond) { }`, and accepts `?T` as an implicit non-null condition |
| `break`, `continue` | **Working** | Unlabeled forms target the innermost active loop. Labels accept `outer: for`, `break outer;`, and `continue outer;`; unknown or duplicate active labels are rejected |
| `return` (void and typed) | **Working** | |
| `for (cond) { }`, `for { }` | **Working** | Conditional and infinite loop forms lower to the same CFG as `while`; labels are supported on both forms. The formatter preserves `for` spelling and `?T` conditions are tested implicitly as non-null |
| `for (init, cond, step) { }` | **Working** | Flat and parenthesized clause forms are accepted. `init` and `step` are both optional; `continue` still runs the step before the next test. Labels are stored on the real `For` node |
| `for (x in xs)` | **Working** | Duck-typed iterator over a struct with `next(self)`; the canonical `next(self): ?T` returns `null` for the iteration end and a payload for each element. `??T` iterators bind the loop variable as `?T` so `null` elements are distinguishable from the end. The legacy tagged-union `{ T, End }` protocol remains accepted during migration. Labels are supported |
| `when` / `match` pattern match | **Working** | Arms are written `(pattern) body`, comma-separated; optional later islands are boolean guards (`(pattern) and (guard) body`). `match` is a parser synonym for `when`. Equality, boolean and range (`1..3`) patterns lower through HIR to codegen, and the first island may combine non-boolean patterns with `and`/`or`/`xor`. An `(f is Member)` arm narrows `f` for that arm's body. `(_)` must be the sole island of the last arm; a value-producing `when` without a default reports non-exhaustive. Legacy `~>` stays accepted but emits `W1008`. Covered by runtime tests |
| `state` / `jump` | **Working** | `state Name(params): ReturnType` declares a state with the machine return type; `jump Next(args)` terminates the current block and validates arity/types against the target's own parameters before a direct `musttail tailcc` transfer |
| `dock` | **Working** | `dock State(args)` is a `tailcc` call expression that returns the machine's final `state` return value; the old `dock { ... }` block form is rejected |
| `defer expr;` scope guards | **Working** | `defer expr;` and `defer { ... }` register cleanup on the nearest lexical block and run in reverse registration order on normal exit, `return`, `break`, `continue`, and `state` `jump`. A defer may capture same-block bindings declared later; exits before initializing a captured binding are rejected. The deferred body is cleanup-only and rejects `return`/`break`/`continue`/`jump` |
| `drop` cleanup hooks | **Rejected for Zith--** | The token remains reserved, but automatic per-owner cleanup is not part of the language design. See [ADR-0055](adr/0055-zith-minusminus-explicit-ownership-and-pipelines.md) |

### Words, Contexts, Macros

| Feature | Status | Notes |
|---|---|---|
| `prefix`, `suffix`, `infix`, `nop` decls | **Parse skipped** | Body skipped via `skipDelimited` |
| `context` declarations | **Parse skipped** | Body skipped |
| `use` statements | **Parse skipped** | Body skipped |
| `macro` / `raw macro` declarations and `@name(...)` calls | **Working** | Normal macros rename template-local bindings hygienically and resolve other template names through the call-site scope (globals/imports visible when not shadowed). `raw macro` splices literally into the call-site scope and names resolve there before module/global fallback. Templates are not analysed as code; resolution is keyed by node id |
| `tag` calls (formerly `tag macro`) | **Parse reported / full Zith** | `<Section ...> ... </Section>` and named attributes parse, but the declaration is rejected in the Zith-- pipeline with `E2010`; `tag` is a full-Zith item, not a Zith-- macro. Use normal or `raw macro` in Zith-- |
| word call expressions | **Parse error** | No parser support |
| word sequence expressions | **Parse error** | No parser support |

### Module System & Visibility

| Feature | Status | Notes |
|---|---|---|
| `import`, `from`, `export` | **Working** | `import path` exposes only the full dotted namespace (`std.io.console.println`), `import path as name` exposes `name.symbol`, `from path` injects public symbols, and `export path` re-exports the namespace plus public symbols |
| `c/...` supported runtime surface | **Working / Partial** | `c/io`, `c/stdlib`, `c/string`, and `c/fs` provide the common declarations used by the shipped stdlib; `c/fs` is the portable C stdio file surface. `c/posix` ships only as the platform variant `c/posix.linux.zith` (POSIX file and `mmap`), so it resolves on Linux and reports the missing-platform-variant diagnostic elsewhere. Native LLVM, no-LLVM VM, and WASM backend resolution remains target-specific; the safe `std/fs` wrapper stays deferred ([ADR-0046](adr/0046-c-file-surface-and-deferred-safe-std-fs.md)) |
| `alias` | **Working** | |
| `pub`, `mod` | **Working** | |
| `mod(..)`, `mod(N)` | **Working** | Module-depth visibility is applied to declarations and to struct fields; `mod(..)` is unlimited and `mod(N)` allows N directory levels below the owner |
| C header imports | **Working (validated C)** | libclang only; variadic functions, array-decayed parameters, and `va_list` are supported. Declarations with function-pointer parameters or results are skipped until the imported type model preserves callable signatures. Object-like scalar macros are imported as constants. Directly declared external scalar and pointer globals, plus reachable typedefs such as `FILE`, are imported; `static` and transitively declared globals are skipped. Top-level C object constness controls binding mutability, not pointee constness. By-value records require a verified layout and are limited to 64-bit x86-64/AArch64 Linux (GNU or musl), using libclang's effective target when no triple is specified. Supported shapes: one i64/pointer or two adjacent i32 slots in 8 bytes; two integer-class slots at offsets 0 and 8 in a 16-byte, 8-aligned record (`i64+i64`, `i32+i64`, or `i64+i32`, with pointers in i64 slots). Other target ABIs and aggregate classes remain unsupported. VM v2 reports a named unsupported-global diagnostic when code references an external global |

---

### Zith Design Surfaces Outside the `Zith--` Contract

These entries are described in Zith design documents but are not part of the
current `Zith--` contract. Listing them here does not make them planned work
for `Zith--`; its planned features are identified in `docs/roadmap.md`.

| Feature | Spec chapter |
|---|---|
| NRA ownership proof (full state machine and proof rules; Zith-- implements only the frozen SRA slice, see the pipeline table) | [nra-spec.md](nra-spec.md) |
| `comptime` evaluation | [11-comptime.md](11-comptime.md) |
| `try` / `try ... or` / `fail` / `with` / `catch` / `must(cond)` assertion / `throw` | [08-error-handling.md](08-error-handling.md) |
| Assets (`ZithProject.toml` asset paths) | [12-assets.md](12-assets.md) |
| `.zirl` binary format | [01-overview (§1.5)](Zith-spec.md) |
| `@appendField`, `@removeField`, `@appendMethod` | [11-comptime.md](11-comptime.md) |

---

## Legacy Reserved Syntax (Not Part of the Core Language Contract)

| Surface | Current behaviour | Notes |
|---|---|---|
| `async fn` | **Parse skipped** | Legacy parser affordance only, and out of scope for the current design. A normal `async`/`await` model is expected to remain banned because suspension points hide ownership/lifetime effects from the NRA. Concurrency is documented as `stdlib`/runtime APIs, not a function kind |
| `yield` | Reserved token | Not a core statement |
| `fork`, `merge`, `revoke`, `spawn` | Spec only (Zith) | Draft thread protocol; no frontend/HIR contract exists yet. See docs/plans/branch-protocol.md |
| `await` | Reserved token | Not a core statement. `async`/`await` is out of scope and expected to remain banned |

---

## CLI Commands

| Command | Status | Notes |
|---|---|---|
| `zithc build` | **Working** | Links an executable into `target/` by default; `--emit obj/ir/asm/hir` stop earlier; `--cache-stats` prints object-cache hit/miss counts. In a package project it requires a valid `Zith.lock` and reports `run 'zithc deps resolve'` when missing or stale |
| `zithc run` | **Working** | Compiles + executes in one step; the program's stdout/stderr is forwarded to zithc's **stdout**, compiler diagnostics stay on stderr. Runs through native codegen by default, through VM v2 when `--virtual-machine` is passed or when the build has no LLVM. Enforces the same `Zith.lock` gate as `build` |
| `zithc check` | **Working** | Type-checks without emitting. Errors forwarded from frontend snapshot. Enforces the same `Zith.lock` gate as `build` |
| `zithc fmt` | **Working** | Round-trip tested for `Index` and `OptionalProp` |
| `zithc create <name>` | **Working** | Scaffolds a project and resolves its empty dependency graph, writing `Zith.lock` so the fresh project passes the `check`/`build`/`run` lockfile gate immediately |
| `zithc clean` | **Working** | |
| `zithc execute <file>` | **Working** | |
| `zithc test <path>` | **Working** | Discovers `.zith` files under `test_dir`, executes selected `#[test]` functions through the native backend, VM v2, or HIR interpreter, and reports per-test results plus failing assert locations. `--filter <substring>` matches case-sensitively against `module::function`. Files without tests are skipped successfully. An empty test directory and a filter with no matches are errors. `--verbose` reports compilation and test-run progress. The runner captures child output to parse result and log frames, suppresses passing-test logs by default, shows all test logs with `--verbose`, and caps displayed log bytes at 4 KiB per test. Rich `std/test` formatting is covered in native and VM v2; the HIR interpreter currently has a gap in that path. |
| `zithc repl` | **Stub** | |
| `zithc deps list` | **Working** | Reads `ZithProject.toml` and lists declared dependencies |
| `zithc deps add <name> <relative-path>` | **Working** | Adds a local path dependency to `ZithProject.toml`; resolution remains explicit |
| `zithc deps remove <name>` | **Working** | Removes a dependency declaration without changing `Zith.lock` |
| `zithc deps resolve` | **Working** | Resolves the transitive local dependency graph and writes a deterministic `Zith.lock` |
| `zithc docs` | **Working** | Deterministic Markdown for reachable modules, interface/spec modes, stdout or safe `--out` output, multipage `--index`, and opt-in partial output with `--error` |
| `zithc completion <shell>` | **Working** | Emits bash, zsh, or fish completion scripts for the current command set |

---

## Diagnostic Codes

Codes are grouped by pipeline stage. `E0000` remains the generic user-reported diagnostic.

| Range | Stage | Codes |
|---|---|---|
| 0001-0005 | Lexical | `E0001` UnknownToken, `E0002` UnclosedString, `E0003` InvalidEscape, `E0004` InvalidIntLiteral, `E0005` UnclosedComment |
| 1001-1010 | Import/parse | `E1001` ExpectedExpr, `E1002` ExpectedSemicolon, `E1003` UnclosedParen, `E1004` ExpectedIdent, `E1005` InvalidImportDepth, `E1006` ImportError, `E1007` TopLevelLetNotAllowed, `W1008` DeprecatedSyntax (`while` -> `for (cond)`; `else if` -> `else (cond)`; `~>` in `when` arms), `E1009` CircularImport, `E1010` ParseDepthExceeded |
| 2001-2010 | Semantic | `E2001` UndefinedIdent, `E2002` DuplicateDecl, `E2003` WrongArity, `E2004` UnusedDecl, `E2005` NotNamespace, `E2006` NoMember, `E2007` NoMatchingFn, `E2008` AmbiguousCall, `E2009` NotImplemented, `E2010` UnsupportedSyntax |
| 2021-2029 | Frontend/interface | Trait requirement/signature checks (`E2021`/`E2022`), `E2023` NotATrait, `E2024` InterfaceNotSatisfied, `E2025` InterfaceMethodNotAllowed, `E2026` DiscardedResult, `E2027` DuplicateImplementation, `W2028` UnknownAttribute, `W2029` AttributeNotApplicable (`#[test]` with parameters or a non-void return) |
| 3001-3009 | Types | `E3001` TypeMismatch, `E3002` CannotInfer, `E3003` InvalidCast, `E3004` CyclicType, `E3005` NullDerefUnproven, `E3006` CoercionFailure, `E3007` WidthMismatch, `E3008` OptionalViolation, `E3009` ConstraintNotSatisfied |
| 4001-4008 | SRA / ownership | `E4001` UseAfterMove (logical receiver or `&x` move in sema), `E4002` BorrowConflict, `E4003` DoubleBorrow, `E4004` WriteThroughView, `E4005` OwnershipCoercionRequired, `E4007` InvalidCallOwnership, `E4008` PointerEscapesScope — call annotations, borrow conflicts and views are checked in sema; direct pointer-local rebinds clear the old alias state; `E4004` remains emitted for views |
| 5001-5002 | Lowering | `E5001` InvalidIR, `E5002` Unreachable |
| 10001-10004 | Runtime | `R10001` IndexOutOfBounds, `R10002` DivisionByZero, `R10003` NullDeref, `R10004` Panic |

---

## Verification
The full inventory retains the evidence from the previous audit at `a5f3716`. On 2026-10-26, the
native test compiler surface was rechecked against the build from `7595e541`: `zithc check`
accepted a valid `#[test]`, `W2029` was emitted for parameterized and non-void tests, `E2010` was
emitted when a module combined `main` with `#[test]`, and `@assert(false)` outside a test trapped.
On 2026-10-27, the local runner build was checked through native, interpreter, and VM v2 execution,
including filtered macro assertions, failing assertions with source locations, helper-function
traps, compilation errors, testless files, and empty test directories. On 2026-10-29, accumulated
`@check` failures, check-helper macro context, check/assert ordering, and the non-boolean diagnostic
were verified. The CLI tests also verify the filter help/completion entries and verbose progress
output.

---

## Known Debt

Recorded deliberately. Each item is a follow-up, not an unknown.

| Item | Notes |
|---|---|
| Non-literal narrowing conversions need runtime checks | Compile-time `as` checks cover literal, `-` literal and integer-const operands; variable operands and implicit numeric-literal adaptation (without `as`) remain unchecked |
| `?*T` pointer narrowing is local-only | Non-null proof currently follows a nullable-pointer local through `is null`/`not (is null)` branches and early-return checks, and a write to the binding invalidates the proof; derived field/index/arrow expressions and cross-function proofs are not yet tracked |
| `null` in a pack literal | Resolved. A surrounding annotation supplies the member type (`let a: \|bytes: ?*u8, len: u64\| = \|null, 7u64\|;`) and `null as ?T` fixes the optional type of a bare `null` at a call site (`new<Buffer>(\|null as ?*u8, 0u64\|)`). A `null` with no type context is rejected in `inferPackLiteral` with `E3001`, so the malformed pack is never interned and cannot reach codegen; a directly bound `let t = \|null, 7u64\|;` used to hang LLVM at 100% CPU |
| `void`/`never` member in a generic union | Resolved. A generic union instantiated with `void` (`union Pair<T> { T, i32 }` used as `Pair<void>`, or `Result<void, E>`) substituted a `void` member that reached codegen and hung LLVM in `getTypeAllocSize`. `instantiateTemplateFromArgs` now rejects a `void`/`never` member with `E3001` at the use site, matching the declaration-path rule. `Result<void, E>` full support (a unit representation for a member) remains a separate, larger item |
| Opaque cache divergence diagnostics | Divergence identifies the canonical type id and recovery path, but does not identify which field changed |

*When a feature moves from one status to another, update this table and re-verify.*

## Non-debt

| Surface | Deliberate Zith-- boundary |
|---|---|
| `is <type>` outside tagged unions and bare `opaque` | General type tests are not part of the Zith-- contract |
| C struct-by-value outside validated layouts | C interop does not promise compatibility with every aggregate ABI. By-value records remain limited to the documented 64-bit Linux target and integer-only layouts; other target ABIs and aggregate classes are deliberately rejected until validated |
| C++ interop | Deliberately parked. The only defensible slice is Itanium name mangling plus standard-layout class ABI with no vtables, RTTI, exceptions, or templates. It starts only after the C ABI surface is complete and verified, and only for a concrete C++ library to call. See [the drawing](plans/abi/cpp-interop-drawing.md) |
| User-defined casts | No user-defined conversion trait or cast syntax is part of the Zith-- spec |
| Bodyless `for (literal..literal){}` | Rejected in sema with a targeted diagnostic before HIR/codegen |
| `..` / `...` | Intentionally one `Dots` token; `..` is the range/pipe/import token and `...` is the variadic marker |
| `++` / `--` | Intentionally rejected; update values with explicit assignment |
| Full NRA ownership proof | Specified for full Zith; Zith-- keeps the implemented residual-fact contract |
