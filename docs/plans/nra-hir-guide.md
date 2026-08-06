# HIR/NRA Implementation Playbook

Operational guide for future ByteAsk agents working on the HIR and NRA boundary in Zith. This
document describes the current source-tree contract: the pipeline order, where residual ownership
facts are stored, the pitfalls discovered in this session, the debugging workflow, and the
validation checklist that keeps the contract stable.

It intentionally does not update code, APIs, or existing documentation. When one of the referenced
implementation facts changes, update this playbook and the docs listed under "Docs That Must Stay
Aligned".

## Stable Pipeline Contract

The documented compiler pipeline is:

```text
source -> lex -> scan -> resolve(import/symbols) -> sema -> comptime/solve -> NTA/NRA -> HIR -> LLVM
```

The current `CompilationSession::runTo` execution order is:

```text
sema -> solve -> nra -> lower -> codegen/cache
```

`sema` builds the modern sema pipeline. `solve` currently creates the modern type table as a
conservative stub. `nra` accumulates `NraFacts` before lowering. `lower` runs `HirLowerModern`,
then runs the current HIR-based `comptime::Solver` as a compatibility pass after lowering. `codegen`
and `cache` run only for stages that require them.

The stable boundary is: the pre-HIR `NTA/NRA` pass accumulates residual ownership facts, and the
final HIR is ownership-free except for those residual side-table facts. Do not move the ownership
proof behind final lowering or invent HIR ownership nodes.

The `Stage` enum is explicit about the target order:

```cpp
Source, Lexed, Scanned, Imported, Resolved, TypeChecked,
Solved, NraResolved, HirLowered, CodegenReady, Cached
```

## Where Ownership Facts Live

- `NraFacts` is the pre-lowering residual ownership accumulator defined in
  `src/sema/nra-facts.hpp` and implemented in `src/sema/nra-facts.cpp`. It is created in
  `CompilationSession::nraStage()` before HIR lowering.
- `HirAttrs` is the parallel side-table owner defined in `src/hir/hir-attrs.hpp`. It attaches
  per-slot, per-call, and per-function facts to otherwise ownership-free HIR.
- `HirLowerModern` receives the facts as `const NraFacts *` through its constructor in
  `src/sema/hir-lower-modern.hpp`.
- `CompilationSession` keeps the facts in `mNraFacts` and exposes them through `nraFacts()`;
  do not replace that accessor without updating callers that inspect residual facts after HIR
  lowering.
- HIR expressions and function records stay free of ownership fields. No `HirMove`, `HirBorrow`, or
  `HirYield` node exists in the current tree. Programs without ownership must produce empty residual
  attribute tables.

## Known Pitfalls

- **Implicit return**: the implicit return value lives in the last `Expression` statement of the
  function body, not in `body.operands`. Treating the body node as `Return` or trusting empty
  `operands` misses forwarding facts and return equivalence.
- **Field/Arrow ownership**: for `Field` and `Arrow`, resolve ownership in this order: direct local
  first, then the `exprTypes` qualified type, then the qualified field type stored on the root
  local's struct type. For `Arrow`, descend through the pointee before looking up the field type.
- **Belong escape guard**: the old `index + 1U <= argument_locals.size()` guard blocked `belong`
  escape facts. Use the zero-based argument slot `index - 1U` and validate against
  `fact.argEscapes.size()`.
- **Sema lifetime**: keep `SemaPipeline` alive through lowering. Do not reset the semantic pipeline
  before NRA/HIR consume it.
- **No ownership HIR nodes**: HIR carries only lazy residual facts. Programs without ownership must
  have empty `HirAttrs` tables.
- **Build constraints**: the project compiles with `-Weverything -Werror` on Clang. Respect
  arena/`DynArray` ownership, avoid exceptions/RTTI, and do not leave unused params/members or NRVO
  warnings.
- **Formatting**: `fmt-check` can fail because of unrelated worktree files. Format only files you
  touched, then run the global check without reverting existing user changes.
- **Debug traces**: remove temporary `fprintf`/`std::cerr`/`std::cout` output before finishing and
  confirm with `rg -n "fprintf|std::cerr|std::cout"`.

## Debugging Workflow

Use focused test targets when iterating on this boundary:

```bash
cmake --build build -j --target test-hir-lower-modern test-memory-qualifiers test-codegen
```

Run the full suite before closing:

```bash
ctest --test-dir build --output-on-failure
```

Useful source files for manual inspection:

- `src/session/compilation-session.cpp` — stage sequencing and fact lifetime.
- `src/sema/nra-facts.cpp` — residual fact accumulation.
- `src/sema/hir-lower-modern.cpp` — HIR emission and side-table population.
- `src/hir/hir-attrs.hpp` — residual fact structs and table access.
- `tests/test-hir-lower-modern.cpp` and `tests/test-memory-qualifiers.cpp` — contract tests.

When a fact or HIR node is missing, first decide whether it belongs before HIR or after HIR. Add a
temporary trace only around the failing stage, run the focused target, and remove the trace before
the final verification.

## Validation Checklist

- Confirm every playbook claim against the current state of `src/sema/nra-facts.*`,
  `src/hir/hir-attrs.hpp`, `src/session/compilation-session.*`, and
  `src/sema/hir-lower-modern.*`.
- Confirm no invented ownership HIR nodes exist. The current node set in
  `src/hir/hir-expr.hpp` has no `HirMove`, `HirBorrow`, or `HirYield`.
- Confirm the documented pipeline matches `CompilationSession::runTo`.
- Run the focused targets, then the full suite, then `fmt-check`.
- Key scenarios must still be covered: HIR without ownership produces empty residual attrs;
  `unique` forwarding sets `returnsArg == 0`; `belong` escape records `Escape`; duplicated
  default/unique/lend vs `share` produce different facts; and `NonNull` narrowing survives as a
  slot fact.

## Docs That Must Stay Aligned

- `docs/Zith-spec-full.md` — the authoritative pipeline and NRA boundary description.
- `docs/impl-status.md` — feature status, including the pre-HIR residual-fact boundary and partial
  F-14.
- `docs/roadmap.md` — F-14 as a partially implemented structure and concurrency as
  `stdlib`/runtime APIs, not core syntax or HIR nodes.
