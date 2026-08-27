# Step 03 — Trait/Interface Conformance

## Goal

After this step, `implement T as Trait` records a verified conformance edge, trait
defaults become callable methods, interfaces are satisfied structurally, and
`Self` resolves correctly in trait/implement contexts. This is the query used by
constraints, introspection and capabilities.

## Prerequisites

- step-01 (member storage).
- step-02 (`requires`/`extends` and `ConstraintRef`).

## Baseline Facts

- `lowerImplementBlock` consumes `Owner as TraitName` and still leaves no trace of
  the trait name in the snapshot.
- `PerModuleSema` has `TraitType`, `internTrait`, `lookupNamed`, and a
  struct-owner method path, but no conformance table.
- Method calls currently require `self->field` inside an implicit `*Owner`
  receiver; `self.x` is a `E3001 field access on non-struct type having type
  '*P'`. A prerequisite fix for implicit receiver auto-deref is expected before or
  during this step.

## Design Contract

1. Add `ConformanceTable` owned by sema/types with query:

   ```cpp
   bool satisfies(TypeId type, TypeId traitOrInterface) const;
   ```

2. `implement T as Trait` produces a conformance record after checking:
   - every trait requirement without a default has a matching method in the impl,
   - method match compares name, arity, parameter types and return type after
     substituting `Self = T`,
   - every `requires` supertrait is satisfied,
   - unknown methods beyond the trait are recorded as extension methods (allowed).
3. Trait defaults are instantiated per implementing type as ordinary owner methods
   in the frontend snapshot, so existing sema/codegen method dispatch works.
4. Interfaces are structural. `satisfies(type, interface)` is true when every
   interface field and type exists on the type. An explicit
   `implement T as SomeInterface` is rejected with `E2025`.
5. `Self` is substituted:
   - in `implement T as Trait`, to `T`,
   - inside trait default bodies, to the implementing type being instantiated,
   - in generic trait bodies, with the generic parameter mapping of the
     instantiated trait.
6. Duplicate `implement T as Trait` is rejected.
7. `@hasTrait` in step-07 and generic constraints in step-05 call this query; no
   other table should be invented.

## Implementation Steps

1. Add `sema::ConformanceTable` next to `TypeTable` in `modern-types.hpp/cpp`.
2. Store the trait name consumed by `lowerImplementBlock` (or add it as a new field
   if the frontend snapshot changed after step-01).
3. Lower the impl methods to sema, resolve `Self`, and type-check requirements
   against the trait's nested functions.
4. Instantiate trait defaults into the implementing owner after the impl's own
   methods so overrides win in method resolution.
5. Add structural interface lookup in `lookupNamed`/field resolution and connect
   it to `ConformanceTable::satisfies`.
6. Add duplicate detection keyed by `(type, trait)`.
7. Emit `E2021`/`E2022`/`E2023`/`E2024`/`E2025`/`E2026`/`E2027` with the
   specification in Diagnostics.

## Diagnostics

Add:

- `E2021 TraitRequirementMissing`: required method absent from impl.
- `E2022 TraitMethodSignatureMismatch`: name matches but arity/params/return differ.
- `E2023 NotATrait`: trait name in `implement ... as` is not a trait.
- `E2024 InterfaceNotSatisfied`: structural interface missing a field or type.
- `E2025 InterfaceMethodNotAllowed`: `fn` in interface, or explicit
  `implement T as Interface`.
- `E2026 RequiresNotSatisfied`: supertrait requirement failed.
- `E2027 DuplicateImplementation`: same `(type, trait)` implemented twice.

## Verification

Add `tests/test-trait-conformance.cpp`:

```zith
trait Printable {
    fn print(self);
    fn describe(self): i32 { return 1; }
}
struct Point { x: i32 }
implement Point as Printable {
    fn print(self) { }
}
fn main(): i32 {
    var p: Point = Point { x: 1 };
    return p.describe();
}
```

Must pass and return 1 at runtime (or `zithc check` for sema-only).

Negative cases, each exactly one code:

```zith
trait Required { fn run(self): i32; }
struct A { x: i32 }
// A missing run -> E2021

interface Pos { [x, y]: f32 }
struct B { x: f32 }
// B missing y -> E2024

struct C { x: i32 }
implement C as Pos {}
// explicit interface impl -> E2025

trait D {}
implement C as D {}
implement C as D {}
// duplicate -> E2027
```

Commands:

```bash
cmake --build build -j --target test-trait-conformance
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `impl-status.md`: traits/interfaces move to Working (static conformance).
- `docs/04-traits-interfaces.md`: mark the trait/interface body and conformance
  section Working; `dyn` remains spec-only.
- `docs/roadmap.md`: F-37 complete; F-29 dependency satisfied.
- `memory/nra-hir-boundary.md`: verify the method-dispatch notes still match after
  auto-deref/conformance changes.
