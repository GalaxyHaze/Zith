# NRA Call Annotations and Ownership Coercions

This step defines the first practical slice of NRA that the compiler should ship
without building the complete alive/dead/lent proof machine. The feature is
intended to be migrated to another branch, so this file documents the syntax,
ownership rules, severity, and the LLVM reference direction agreed so far.

## Goal

Add explicit ownership annotations on function-call arguments (`f(lend x)`,
`f(view x)`, `f(share x)`), validate ownership coercions in sema, and restrict
`belong` to struct fields. Literals do not require annotations. The changes must
keep the stable pre-HIR NRA boundary from `memory/nra-hir-boundary.md`; this step
does not implement the full NRA proof state machine.

## Syntax

Use the prefix form at the call site:

```zith
fn update(p: lend Point) { p.x += 1; }
fn read(p: view Point) { return p.x; }
fn shareConfig(p: share Config) { }

var q: Point = ...;
update(lend q);
read(view q);
shareConfig(share q);
```

Invalid call-site annotations include `f(unique x)` and `f(belong x)`.
Annotations are accepted only for call arguments; using one elsewhere is a
diagnostic.

## Ownership Coercion Rules

Implement the following matrix in call argument checking:

| Source value | Target parameter | Required call annotation |
|---|---|---|
| `belong` field | `lend` | none (`belong` coerces to `lend`) |
| `share` | `share` | none |
| `share` | `lend` or `view` | none |
| `default` or `unique` | `lend` or `view` | `lend` or `view` |
| `default` or `unique` | `share` | `share` |
| literal | `lend`, `view`, or `share` | none |
| call result / temporary | `lend`, `view`, or `share` | none |

`belong` is valid only on struct fields. A `belong` on a local binding,
function parameter, return type, or other declaration position is rejected and
is coerced automatically only when the field is passed as a `lend` argument.

## Diagnostics

New ownership diagnostics should live in the reserved `4001-4999` range and
behave as hard errors:

- `E4005 OwnershipCoercionRequired`: a `default`/`unique` value was passed to a
  `lend`/`view`/`share` parameter without the required annotation.
- `E4006 BelongOnlyInStruct`: `belong` was used outside a struct field.
- `E4007 InvalidCallOwnership`: an annotation such as `unique` or `belong` was
  used at a call site, or an annotation appeared outside a call argument.

Update `src/diagnostics/error-codes.hpp` and
`src/diagnostics/error-codes.cpp` with these codes before use.

## Implementation Shape

1. Add an ownership-coercion expression node to the frontend, e.g.
   `ExprKind::OwnershipCoerce`, carrying the parsed `OwnershipKind`. Parse only
   `lend`/`view`/`share` prefixes in call-argument position.
2. Update sema argument checking for free functions, methods, and overload
   selection so the ownership matrix is checked before normal type coercion.
   Keep `resolve()`/`sameType()` transparent to qualifiers so implementations do
   not accidentally change plain type matching.
3. Keep the existing `NraFacts` accumulator contract. The annotated argument
   lowers as its inner operand; `NraCallFact` continues to own escape/move/share
   residual facts and `HirLowerModern` still emits `HirAttrs` side tables only.
4. Update the formatter/printer and tests for the new node. HIR and cache format
   should not change unless the reference lowering below is included.

## LLVM Reference Direction

Today `CodeGenType::lowerPtr` ignores the `is_mut` bit and `lend T`/`view T`
lower to the same value type as `T`. The agreed direction is to make `lend` and
`view` real references at the HIR/codegen boundary:

- `view T` lowers as a LLVM `ptr` with read-only semantics. In LLVM terms this
  means adding parameter attributes such as `readonly` and `nocapture`. LLVM has
  no immutable pointer type, so `readonly` is the enforcement model.
- `lend T` lowers as a LLVM `ptr` with mutable access, plus `nocapture`.
- Call sites materialize the reference through HIR `Ref`/`Deref`, reusing the
  existing `emitAddrOf` spill path for literals and call results that need a
  stable temporary.
- `share` is not a reference borrow in this step; it remains a value/ownership
  category unless a later step explicitly redefines it.

This reference lowering is a substantial ABI change and is intentionally
documented as the follow-up direction, not as work required by the first syntax
and sema slice.

## Test Plan

Add coverage to `tests/test-memory-qualifiers.cpp` for both accepted and
rejected cases:

Accepted:

```zith
struct P { x: i32, parent: ?belong Self }
fn update(p: lend P);  update(lend q);
fn read(p: view P);    read(view q);
fn take(p: share P);   take(share q);
fn useParent(p: lend P); useParent(q.parent);
fn lit(p: lend i32);   lit(42);
```

Rejected:

```zith
fn update(p: lend P); update(q);          // E4005
fn read(p: view P);   read(q);            // E4005
fn take(p: share P);  take(q);            // E4005
fn bad(p: belong P);  // E4006
update(unique q);     // E4007
```

Run `cmake --build build -j`, `ctest --test-dir build --output-on-failure`, and
the formatter round-trip check on the touched files.

## Documentation To Update

- `docs/impl-status.md`: mark NRA ownership annotations/coercions as working or
  in progress and keep the existing residual-fact note.
- `docs/07-memory-model.md`: document call-site annotation syntax and the
  ownership coercion matrix.
- `docs/roadmap.md`: update F-14 status to reflect the implemented slice.
- `memory/README.md`: add a durable memory note for the migration branch.

## Assumptions

- Ownership annotation syntax: prefix at the call site, `f(lend x)`.
- `belong` is restricted to struct fields and coerces automatically to `lend`.
- New violations are hard errors (`E400x`).
- This step does not implement the full alive/dead/lent machine; it only makes
  the annotation and coercion layer explicit and keeps residual facts aligned.
