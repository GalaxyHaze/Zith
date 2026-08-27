# Zith Compiler Roadmap

> Derived from [Zith-spec.md](Zith-spec.md) and [impl-status.md](impl-status.md) at baseline
> `bf7925e`. Each feature carries a stable ID (`F-NN`) reused across waves and test suites.

## Feature IDs

| ID   | Feature | Spec Chapter | Status (Wave 01) |
|---|---|---|---|
| F-01 | `when` pattern matching | 3, 9 | WIP (not committed) |
| F-02 | `match` expression | 9 | Parse error |
| F-03 | `for` iterator (`in`) and 3-clause | 9 | Parse error |
| F-04 | `state` machine / `dock` / `jump` | 9 | Working (musttail transitions) |
| F-05 | `const fn` | 5, 11 | Parse-level in progress |
| F-06 | `is <type>` type narrowing | 3 | Parse error |
| F-07 | `dyn Trait` dynamic dispatch | 14 | Parse error |
| F-08 | `@macro` calls | 15 | Working |
| F-09 | Word call and sequence expressions | 16 | Parse error |
| F-10 | `@sizeOf`, `@intrinsic` expressions | 11 | Parse error |
| F-11 | `fail` / `with` / `catch` / `throw` / `must` | 8 | Spec only |
| F-12 | `T!` failable propagation (`!` postfix) | 8 | Spec only |
| F-13 | `raw` unwrap operator | 13 | Spec only |
| F-14 | NRA/NTA ownership proof before stable HIR (`lend`, `view`, `unique`, `share`, `belong`) | 7 | In progress (residual-fact contract implemented in Wave 05; full rule diagnostics remain) |
| F-15 | `comptime` blocks | 11 | Spec only |
| F-16 | `const fn` compile-time evaluation | 11 | Spec only |
| F-17 | Reflection intrinsics (`@fields`, `@hasTrait`, `@appendField`, ...) | 11 | Spec only |
| F-18 | Runtime task/coroutine APIs (`Task<T>`, scheduling, join/blocking helpers) | 10, 20 | Spec only (stdlib/runtime, not core syntax) |
| F-19 | Runtime thread/channel APIs and resource wrappers | 10, 20 | Spec only (stdlib/runtime, not frontend syntax) |
| F-20 | NRA shared-resource facts for runtime concurrency APIs | 7, 10 | Spec only (pre-HIR semantic dependency) |
| F-21 | `context` block semantics | 17 | Parse skipped |
| F-22 | `use` statement semantics | 2, 17 | Parse skipped |
| F-23 | `prefix` / `suffix` / `infix` / `nop` semantics | 16 | Parse skipped |
| F-24 | Tag macros (`<Tag>`) | 15 | Working |
| F-25 | Assets (`ZithProject.toml` asset paths) | 12 | Spec only |
| F-26 | `::` scope resolution | 2 | Spec only (normal/raw macro scope diversification is complete; `::` remains a separate wave) |
| F-27 | Binding destructuring (`[]`) and pack literals | 6 | Spec only |
| F-28 | `@pack` / `@toStruct` / `@toPack` | 11 | Spec only |
| F-29 | Generic trait and interface constraints (`T: Trait`, `T: Interface`) | 4 | Working; interface bounds expose interface fields and methods |
| F-30 | Standard library (beyond io) | 20 | Spec only |
| F-31 | `union` runtime semantics | 3 | Spec only |
| F-32 | C header import completion (macros, variadics, callbacks) | 18 | Working (common C); macros/globals/bitfields remain |
| F-33 | Function overloading (selection by arity and parameter types) | 5 | Working |
| F-34 | Memory qualifier parsing and typing (`mut`, `lend`, `view`, `unique`, `share`, `belong`) | 7 | Working |
| F-35 | Trait and interface bodies as real declarations | 4 | Working; interface bodies accept single/grouped fields and declaration-only method requirements |
| F-40 | Scope-guard `defer` statements | 9, Zith-- | Working |
| F-41 | Deterministic `drop` cleanup hooks | 9, Zith-- | Spec only; depends on F-40 |

## Dependency Graph

```
F-01 (when) ─────────────────────────────┐
F-02 (match)                              │
F-03 (for iter/3-clause)                  │
F-04 (dock)                               │
F-05 (const fn parse)                     │
F-06 (is <type>)                          │
F-07 (dyn Trait)                          ├─ needs F-01
F-09 (word call/seq)                      │
F-10 (sizeOf/intrinsic)                   │
F-08 (@macro)                             │
                                          │
F-11 (fail/with/catch)                    ├─ needs T! types ✓
F-12 (failable propagation)               │
F-13 (raw unwrap)                         │
                                          │
F-21 (context semantics) ──┬── F-22 (use) ├─ needs words
F-23 (word semantics) ─────┘              │
F-24 (tag macros)                         │
                                          │
F-15 (comptime blocks) ───┬─ F-16 (const fn eval) ── needs F-05
F-17 (reflection) ────────┤
F-28 (pack ops) ──────────┘
                                          │
F-34 (qualifier parse/types) ── F-14 (NRA/NTA) ── stable HIR contract
F-33 (overloading)                        ┼─ done (name resolution + linkage names)
                                          │
F-30 (stdlib) ─────────────┬─ F-18 (runtime tasks/coroutines)
                           ├─ F-19 (runtime threads/channels)
F-14 (NRA/NTA) ────────────┘
                           └─ F-20 (shared-resource facts for runtime concurrency APIs)
F-25 (assets) ───────────────────────────────────│
F-31 (union runtime) ────────────────────────────│
F-32 (C interop completion) ─────────────────────│
```

## Wave Groupings

### Wave 02 — Pattern Matching & Control Flow Completion
F-01 (`when` — finish WIP integration), F-02 (`match`), F-03 (`for` iterator/3-clause), F-04 (`dock`), F-05 (`const fn` parse), F-06 (`is <type>`).

Dependencies: none outside the wave.  Max parallelism: 2 agents (F-01/F-02 shared AST; F-03/F-04 independent).

### Wave 03 — Error Handling
F-11 (`fail`/`with`/`catch`/`throw`/`must`), F-12 (`T!` propagation), F-13 (`raw` unwrap).

Dependencies: `?T` and `T!` types already working.  Max parallelism: 2 agents.

### Wave 04 — Comptime
F-10 (`@sizeOf`/`@intrinsic` parse), F-15 (`comptime` blocks), F-16 (`const fn` evaluation), F-17 (reflection intrinsics), F-28 (pack ops).

Dependencies: needs F-05 (`const fn` parsed).  Max parallelism: 2 agents (comptime evaluator + reflection intrinsics).

### Wave 05 — NRA Ownership Analysis
F-14 (`lend`/`view`/`unique`/`share`/`belong` analysis pass), F-27 (destructuring).

The stable HIR boundary is now structural: NTA/NRA facts are accumulated before final lowering and
HIR carries only residual side-table facts. Remaining work is the alive/dead/lent state machine,
the four NRA rule diagnostics, and any further lowering that must preserve qualifier structure
before the proof. Qualifier parsing/typing (F-34) is complete. Max parallelism: 1 agent (single
analysis pass with a tight contract to lowering).

### Completed outside the wave order
F-33 (function overloading) and F-34 (memory qualifier parsing and typing) are implemented.

### Wave 06 — Dynamic Dispatch
F-07 (`dyn Trait`), F-29 (trait constraints `T: Trait`).

Dependencies: needs working generic instantiation (F-38, step-04).  Max parallelism: 1 agent.

### Wave 07 — Macros, Words & Contexts
F-08 (`@macro` calls) and F-24 (tag macros) are implemented. Normal macros keep hygiene for template bindings and resolve other names through the call-site scope; raw macros splice literally into the call-site scope with module/global fallback. Remaining work in this wave: F-09 (word call/sequence expressions), F-21 (context semantics), F-22 (`use` semantics), F-23 (word decl semantics), F-26 (`::` scope resolution).

Dependencies: the macro expander is complete; word/context semantics still need F-09 words parsed first.

### Wave 08 — Runtime Concurrency Surface
F-18 (runtime task/coroutine APIs), F-19 (runtime thread/channel APIs), F-20 (NRA facts needed by
those APIs).

Dependencies: F-30 (`stdlib`) plus F-14 (pre-HIR ownership proof). No frontend syntax or HIR node
work is planned here. Max parallelism: 2 agents (runtime surface + ownership integration).

### Wave 09 — Assets & Stdlib
F-25 (assets), F-30 (stdlib), F-31 (union runtime semantics), F-32 (C interop completion).

Max parallelism: 2 agents (assets + stdlib).

### Wave 10 — Scoped Cleanup

F-40 (`defer`) is implemented in `Zith--`: parser, sema, HIR, and codegen
support `defer expr;` and `defer { ... }` with reverse-order cleanup on block
exit, including `return`, `break`, `continue`, and `state` `jump`. F-41
(`drop`) remains pending and will reuse the same scope-cleanup structure to add
the deterministic owner method plus ownership checks. Max parallelism: 1 agent,
because F-41 is a natural extension of F-40 and both touch the same statement
and block-lowering paths.

## Wave 01 (Current)

Infrastructure: ZIRL sections, cache hydration, CLI commands (`test`, `deps`, `docs`), diagnostics quality, roadmap, and infra tests.  No language-chain changes.  See [impl-status.md](impl-status.md) and [docs/plans/0.7.0/README.md](0.7.0/README.md).

## Notes

- Feature IDs are stable and should be referenced in commit messages, test names, and PR descriptions.
- The dependency graph encodes the minimum build order; waves can overlap when dependencies are acyclic.
- The WIP `when` (F-01) is the first item of Wave 02 and remains uncommitted through Wave 01.
