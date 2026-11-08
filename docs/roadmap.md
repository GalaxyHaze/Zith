# Zith-- Compiler Roadmap

> Tracks compiler work for `Zith--`, a sibling language to Zith. The language
> contracts are separate. This roadmap uses [Zith-spec.md](Zith-spec.md) as a
> comparison reference and [impl-status.md](impl-status.md) for current
> `Zith--` behavior. Each feature carries a stable ID (`F-NN`).

## Feature IDs

| ID   | Feature | Spec Chapter | Status (Wave 01) |
|---|---|---|---|
| F-01 | `when` pattern matching | 3, 9 | Working |
| F-02 | `match` expression | 9 | Working (`when` synonym) |
| F-03 | `for` iterator (`in`) and 3-clause | 9 | Working |
| F-04 | `state` machine / `dock` / `jump` | 9 | Working (musttail transitions) |
| F-05 | `const fn` | 5, 11 | Parse-level in progress |
| F-06 | `is <type>` type narrowing | 3 | Working (tagged unions + opaque; general type tests are not part of Zith--) |
| F-07 | `dyn Trait` dynamic dispatch | 14 | Working (`dyn Interface` also dispatches through vtables) |
| F-08 | `@macro` calls | 15 | Working |
| F-09 | Word call and sequence expressions | 16 | Parse error |
| F-10 | `@sizeOf`, `@intrinsic` expressions | 11 | Working (layout intrinsics); generic `@intrinsic` remains full-Zith |
| F-11 | `fail` / `with` / `catch` / `throw` / `must` | 8 | Spec only |
| F-12 | `T!` failable propagation (`!` postfix) | 8 | Full Zith only: `T!` and `!` propagation remain outside Zith-- |
| F-13 | `raw` unwrap operator | 13 | Working for optional extraction; `unsafe`/raw-block surface remains full-Zith |
| F-14 | NRA ownership proof before stable HIR (`lend`, `view`, `own`, `share`, `belong`) | 7 | In progress (residual-fact contract implemented in Wave 05; full rule diagnostics remain) |
| F-15 | `comptime` blocks | 11 | Spec only |
| F-16 | `const fn` compile-time evaluation | 11 | Spec only |
| F-17 | Reflection intrinsics (`@fields`, `@hasTrait`, `@appendField`, ...) | 11 | Spec only |
| F-18 | Runtime task/coroutine APIs (`Task<T>`, scheduling, join/blocking helpers) | 10, 20 | Spec only (stdlib/runtime, not core syntax) |
| F-19 | Runtime thread/channel APIs and resource wrappers | 10, 20 | Spec only (stdlib/runtime, not frontend syntax) |
| F-20 | NRA shared-resource facts for runtime concurrency APIs | 7, 10 | Spec only (pre-HIR semantic dependency) |
| F-21 | `context` block semantics | 17 | Parse skipped |
| F-22 | `use` statement semantics | 2, 17 | Parse skipped |
| F-23 | `prefix` / `suffix` / `infix` / `nop` semantics | 16 | Parse skipped |
| F-24 | Full-Zith tag items (`@<Tag> ... </Tag>`) | 15 | Design direction accepted; not implemented. Zith-- parses the legacy `<Tag>` call form and rejects the declaration with `E2010` |
| F-25 | Assets (`ZithProject.toml` asset paths) | 12 | Spec only |
| F-26 | `::` scope resolution | 2 | Spec only (normal/raw macro scope diversification is complete; `::` remains a separate wave) |
| F-27 | Binding destructuring (`[]`) and pack literals | 6 | Spec only |
| F-28 | `@pack` / `@toStruct` / `@toPack` | 11 | Spec only |
| F-29 | Generic trait and interface constraints (`T: Trait`, `T: Interface`) | 4 | Working; interface bounds expose interface fields and methods |
| F-30 | Standard library (beyond io) | 20 | Spec only |
| F-31 | `union` runtime semantics | 3 | Partial: tagged unions and `is` narrowing work; remaining union surface is tracked in debt |
| F-32 | C header import completion (macros, variadics, callbacks) | 18 | Working (validated C); simple records by value are imported only after layout validation; advanced macros/globals/bitfields remain debt |
| F-33 | Function overloading (selection by arity and parameter types) | 5 | Working |
| F-36 | Variadic slices (`[...]T` homogeneous tail parameters) | 5 | Working |
| F-34 | Memory qualifier parsing and typing (`mut`, `lend`, `view`, `own`, `share`, `belong`) | 7 | Working |
| F-35 | Trait and interface bodies as real declarations | 4 | Working; interface bodies accept single/grouped fields and declaration-only method requirements |
| F-40 | Scope-guard `defer` statements | 9, Zith-- | Working |
| F-41 | Deterministic `drop` cleanup hooks | 9, Zith-- | Rejected for Zith-- by ADR-0055 |
| F-42 | Platform-specific imports (`foo.<arch>.<os>.zith`) | 2, Zith-- | Working; resolver checks `arch.os`, `arch`, `os`, then generic `foo.zith` |
| F-43 | Native test surface (`#[test]`, `@assert`, `@check`, `@log`, synthesized driver) | 9, Zith-- | Partial: accumulating checks and rich `std/test` formatting pass on native and VM v2; HIR covers checks, portable assertions, and logs, but rich formatting remains unsupported |

`Spec only` marks material described in Zith design documents without a
corresponding `Zith--` implementation. It is not automatically planned work for
`Zith--`; the active waves below identify work intended for that language.

## Dependency Graph

```
F-01-F-04, F-06, F-08, F-10 layout, F-13, F-29,
F-32 validated C, F-33, F-34, F-35, F-36, F-40 ────────────────────── done
(see impl-status.md)
F-43 test surface ─────────────────────────────────────────────────── partial

F-05 (const fn parse) ─────────────────────┐
F-11 (fail/with/catch)                     ├─ needs T! propagation (full Zith)
F-12 (failable propagation)                │
F-14 (NRA full proof)                      │
F-15-F-17, F-20, F-28 ────────────────── full-Zith only
F-21 (context semantics) ──┬── F-22 (use) ├─ needs words
F-23 (word semantics) ─────┘              │
F-24 (`tag`) ────────────────────────────── full Zith only; Zith-- rejects E2010
F-25 (assets), F-30, F-31 ─────────────── remaining spec/debt work
F-18/F-19, F-27, F-26, F-09 ────────────── full-Zith/spec-only
F-34 (qualifier parse/types) ── F-14 (NRA) ── stable HIR contract
F-33 (overloading)                        ┼─ done (name resolution + linkage names)
F-42 (platform imports)                   ┼─ done (resolver-only; independent of comptime)
                                          │
F-30 (stdlib) ─────────────┬─ F-18 (runtime tasks/coroutines)
                           ├─ F-19 (runtime threads/channels)
F-14 (NRA) ─────────────────┘
                           └─ F-20 (shared-resource facts for runtime concurrency APIs)
F-25 (assets) ───────────────────────────────────│
F-31 (union runtime) ────────────────────────────│
F-32 (C interop completion) ─────────────────────│
```

## Wave Groupings

### Wave 02 — Pattern Matching & Control Flow Completion
The Wave 02 surface is implemented in Zith--: F-01 (`when`), F-02 (`match`),
F-03 (`for` iterator/3-clause), F-04 (`dock`), and F-06 (`is <type>`).
F-05 (`const fn`) remains parse-level only; compile-time evaluation is a
full-Zith feature and stays outside the Zith-- core.

### Wave 03 — Error Handling
F-13 (`raw` optional extraction) is working. F-12 is full-Zith only: `T!`
declarations and `!` propagation are outside Zith--. F-11
(`fail`/`with`/`catch`/`throw`/`must` assertion) remains full-Zith-only.

### Wave 04 — Comptime
F-10 layout intrinsics (`@sizeOf`, `@offsetOf`, `@alignOf`, `@lengthOf`,
`@ptrOf`, `@canonicalType`) are working in Zith--. F-15 (`comptime` blocks),
F-16 (`const fn` evaluation), F-17 (reflection intrinsics), and F-28 (pack ops)
remain archived full-Zith features.

### Wave 05 — SRA Ownership Analysis (Zith-- slice, frozen)
F-14 (`lend`/`view`/`own`/`share`/`belong` analysis pass), F-27 (destructuring).

The stable HIR boundary is structural: NIA numeric facts and NRA ownership facts are accumulated
before final lowering and HIR carries only residual side-table facts. The Zith-- portion of this
wave is SRA (Small Resource Analysis) and is frozen at the implemented `lend`/`view`
call-annotation slice; it will not grow into the full proof. The full ownership proof is
full-Zith work, specified in
[nra-spec.md](nra-spec.md) (node state `uninitialized`/`taken`/`ok`, edge state, flow, and rules
NRA-1..NRA-11). Qualifier parsing/typing (F-34) is complete. Max parallelism: 1 agent (single
analysis pass with a tight contract to lowering).

### Completed outside the wave order
F-33 (function overloading) and F-34 (memory qualifier parsing and typing) are implemented.

### Wave 06 — Dynamic Dispatch
F-07 (`dyn Trait`) and the same vtable lowering used by `dyn Interface` are **working**.
F-29 (trait constraints `T: Trait`) is covered by generic constraints.

F-07 now has codegen coverage for nominal traits. Interfaces use the same HIR/codegen path.

### Wave 07 — Macros, Words & Contexts
F-08 (`@macro` calls) is implemented in Zith--. Normal macros keep hygiene for
template bindings and resolve other names through the call-site scope. Raw
macros splice literally into the call-site scope with module/global fallback.
F-24 is a full-Zith-only tag feature. Its design uses `@<Tag> ... </Tag>`, and
it is not implemented. Zith-- retains separate legacy `<Tag>` call parsing and
rejects tag declarations with `E2010`.
Remaining work in this wave: F-09 (word call/sequence
expressions), F-21 (context semantics), F-22 (`use` semantics), F-23 (word decl
semantics), and F-26 (`::` scope resolution).

Dependencies: the Zith-- macro expander is complete. Word/context semantics still need F-09 words parsed first.

### Wave 08 — Runtime Concurrency Surface
F-18 (runtime task/coroutine APIs), F-19 (runtime thread/channel APIs), F-20 (NRA facts needed by
those APIs).

Dependencies: F-30 (`stdlib`) plus F-14 (pre-HIR ownership proof). No frontend syntax or HIR node
work is planned here. Max parallelism: 2 agents (runtime surface + ownership integration).

### Wave 09 — Assets & Stdlib
F-32 (C interop) is working for validated common C plus simple records whose
layout libclang proves; the remaining macros/globals/bitfields are debt. F-31
is partial: tagged unions and `is` narrowing are implemented.
F-25 (assets) and F-30 (stdlib beyond the shipped io/alloc surface) remain
full-Zith/spec-only work.

Max parallelism: 2 agents (assets + stdlib).

### Wave 10 — Scoped Cleanup

F-40 (`defer`) is implemented in `Zith--`: parser, sema, HIR, and codegen
support `defer expr;` and `defer { ... }` with reverse-order cleanup on block
exit, including `return`, `break`, `continue`, and `state` `jump`. F-41
(`drop`) is rejected by ADR-0055. Zith-- keeps owner cleanup explicit.

## Wave 01 (Current Zith--)

Zith--: platform imports (F-42) are implemented as resolver work. Automatic
`drop` cleanup (F-41) is rejected by ADR-0055. Other work remains tracked by
its active plans and debt records without an order implied by this note.
Comptime/introspection/type construction remain full-Zith features and are
archived under `docs/plans/archive/0.7.0-zith/`.

The test surface (F-43: `#[test]`, `@assert`, `@check`, `@log`, the
compiler-synthesized HIR driver, the `zithc test` runner, and `std/test`) runs
its rich formatting and accumulating-check fixtures on native and VM v2. The
HIR interpreter has check, portable assertion, and log coverage, while the
rich formatting fixture remains an open backend gap.

Infrastructure: ZIRL sections, cache hydration, CLI commands (`test`, `deps`, `docs`), diagnostics quality, roadmap, and infra tests.  See [impl-status.md](impl-status.md), [docs/plans/0.7.0/README.md](0.7.0/README.md), and [docs/plans/monolith-splits.md](plans/monolith-splits.md).

## Notes

- Feature IDs are stable and should be referenced in commit messages, test names, and PR descriptions.
- The dependency graph encodes the minimum build order. Waves can overlap when dependencies are acyclic.
- F-01 through F-04 and F-06 are implemented in Zith-- and verified in
  `docs/impl-status.md`; do not treat them as pending roadmap work.
