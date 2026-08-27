# Step 02 — Requires And Extends

## Goal

After this step, `requires Cond` before a trait/interface declaration and `extends`
on a trait are real frontend data: parsed, resolved enough for sema to name the
referenced trait/interface, and available to later conformance and constraints.

## Prerequisites

- step-01 (trait bodies must be parsed before requires makes sense).

## Baseline Facts

- `requires Printable` is currently an unhandled top-level token and reports
  `unexpected token at top level` (`E2010`).
- `extends` is lexed as a keyword but has no declaration path.
- `GenericParam::constraint` stores only a single `TypeExprId`; `requires` and
  `extends` need the same reference shape.

## Design Contract

1. Add `std::vector<ConstraintRef> requiresClauses` and
   `std::vector<ConstraintRef> extendsClauses` to `frontend::Declaration`.
2. `ConstraintRef` stores:
   - the source name (`std::string_view`),
   - the resolved declaration id after import/symbol resolution,
   - the comptime kind predicate if written as `@isStruct`, `@struct`,
     `@component`, `@union`, `@enum`, `@primitive`, or `@nullable`.
3. `requires X` is allowed only before `trait` or `interface`. An error appears
   otherwise.
4. `trait Safe extends Base` allows one or more comma-separated supertraits.
   `interface` cannot extend, matching the spec.
5. `requires` and `extends` do not perform conformance checking in this step.

## Implementation Steps

1. Add the two vectors and `ConstraintRef` to `frontend.hpp`.
2. In `AstLowerer::run`, recognize `requires` before a declaration and set the
   flag/vet the next decl kind.
3. Parse the `requires` list and `extends` list in `lowerDeclaration`.
4. Map the constraint list through import/name resolution in
   `src/session/frontend-context.cpp`.
5. Add formatter output for `requires` and `extends`.
6. Add cache records for both fields; keep serialization deterministic.

## Diagnostics

Add only:

- `RequiresNotSatisfied` (`E2026`, value `2026`): a compilable reference exists
  but the required trait/interface is not satisfied. This step may emit it for
  unresolved names with a "not found" note; conformance logic lands in step-03.

Do not add new codes in this step beyond `E2026`.

## Verification

Add `tests/test-trait-requires.cpp`:

```zith
requires Printable
trait Json { fn toJson(self): string; }

requires @isStruct
interface Positioned { [x, y]: f32 }
```

Assert that parsing succeeds and the declaration carries two constraint refs.

Negative:

```zith
requires @isStruct
struct Foo { x: i32 }
```

Must report a single error explaining `requires` is only valid before
`trait`/`interface`.

Commands:

```bash
cmake --build build -j --target test-trait-requires
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `docs/04-traits-interfaces.md`: status header says `requires`/`extends` now parse
  and store constraints; conformance stays pending.
- `impl-status.md`: rename the `traits/interfaces` row status from parse-only to
  "parsed; conformance pending".
- `docs/roadmap.md`: F-36 complete after this step.
