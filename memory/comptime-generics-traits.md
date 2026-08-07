# Comptime, Generics, Traits & Capabilities 0.7.0

This note records the durable planning contract for Zith 0.7.0: what the release
covers, where the step-by-step implementation guides live, and which design
decisions are locked. It is not a replacement for the detailed step files in
`docs/plans/0.7.0/`; it is the short version a future session should read before
picking a step.

## Locked Scope

- Static dispatch only. `dyn Trait`, vtables and object safety are deferred to
  a later release, but the dedicated "dynamic dispatch is not implemented yet"
  diagnostic is part of 0.7.0.
- Real generic monomorphization before HIR, restoring the documented pipeline
  `sema -> comptime/solve -> NTA/NRA -> HIR`.
- Traits and interfaces become semantically real: trait bodies, default methods,
  `requires`, `extends`, structural interfaces, and conformance checking.
- Static introspection: `@fields`, `@hasTrait`, kind predicates, comptime
  constants for layout intrinsics, and comptime-unrolled `for (f in @fields T)`.
- Full CTFE including `const { }`, `const fn`, and type construction with
  `@struct` / `@appendField` / `@removeField` / `@appendMethod`.
- Capabilities ship as a base only: registry, reserved names, shape validation,
  and an empty behavior slot. No capability changes compiler behavior in 0.7.0.
- `Copy` is not a capability. Implicit bitwise copy is a type-system property,
  not a trait.

## Architecture Facts

`PerModuleSema` resolves generic parameter names to `TypeKind::GenericParam`
entries and stores them in `genericParams_` with declaration id as the key. The
current `comptime::Solver` runs after HIR lowering in
`src/session/compilation-session.cpp` and rejects generic calls with `E3001`
"generic parameter T has no concrete type"; that pass is meant to move before
`nraStage()`.

The parser skips trait/interface bodies with `skipDelimited`, stores generic
constraints only as `TypeExprId constraint` on `GenericParam`, and
`lowerImplementBlock` discards the trait name after consuming it. Sema has
`TraitType`, `internTrait`, `lookupNamed`, and an owner-based method path, but no
conformance table yet.

## Step Files

Each step is a self-contained implementation guide under `docs/plans/0.7.0/`.
Follow the dependency order in that README; the important dependencies are:

1. Trait and interface bodies (parser, formatter, cache) must exist before
   `requires`/`extends` or conformance checking.
2. Conformance must exist before constraints, introspection and capability
   shape validation.
3. Generic monomorphization must exist before constraint enforcement at calls.
4. Monomorphization and conformance must exist before the comptime interned
   type-values that delegate back to them.
5. Capability base must exist after conformance but is intentionally behavior-free.

## Diagnostic Reservation

The full diagnostic list is in `docs/plans/0.7.0/README.md`. The reserved ranges
are semantic `2021-2029` (traits/interfaces/capabilities), generic
`3009-3012`, and comptime `6001-6006`. Parallel sessions must take codes only
from the step that owns them.

## Extension Recipe For Capabilities

To activate a capability later, one future session must add the method signature
set to the registry, then wire the single documented hook in the corresponding
step file: binary operator resolution for `Arithmetic`, `a[i]` resolution for
`Index`, `for (x in xs)` for `Iterator`, and `in` in condition position for
`Range`. With `for`, `in` means iteration; with `if`, it means membership.

`Null` and `Fail` are negative capabilities and are blocked on NRA proof; they are
registry entries only until then.
