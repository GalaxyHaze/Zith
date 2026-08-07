# Step 04 — Pre-HIR Generic Monomorphization

## Goal

After this step, `id<i32>(3)`, inferred `id(3)`, `Pair<T, U>`, and
`implement Node<T>` compile and produce per-argument instances. The solver runs
between sema and NRA, matching the documented pipeline.

## Prerequisites

- step-03 is not required for pure substitution, but conformance-facing
  instantiations should be integrated after it.

## Baseline Facts

- `PerModuleSema` records generic bindings in `genericParams_` and allows `T`
  inside a generic declaration to resolve to `TypeKind::GenericParam`.
- `PerModuleSema::typeContainsGeneric` prevents calls today; a generic call
  reports `E3001: generic parameter T has no concrete type`.
- `comptime::Solver` runs after HIR lowering in
  [compilation-session.cpp](/home/diogo/Zith/src/session/compilation-session.cpp:704),
  contradicting the documented `sema -> comptime/solve -> NTA/NRA -> HIR` order.
- Cache already serializes `CompactTypeKind::GenericParam` and
  `GenericParamRecord`, which can be extended for instantiation records.

## Design Contract

1. Introduce `src/comptime/instantiate.hpp/cpp` with:

   ```cpp
   struct InstantiationKey {
       frontend::DeclId decl;
       MemoryArena *arena;
       std::vector<types::TypeId> args;
   };
   class GenericInstantiationPass {
       bool run(const FrontendSnapshot &snap, TypeTable &types, SymbolTable &syms,
                DiagnosticEngine &diags, MemoryArena &arena);
   };
   ```

2. Placement in `CompilationSession::solveStage()`:

   ```text
   sema -> comptime/solve -> nra -> lower
   ```

   collecting and instantiating generics before `nraStage()`.
3. Collect generic declarations from `frontend::Declaration` where
   `genericParams` is non-empty.
4. At each call/type use:
   - explicit `<...>` wins and is arity-checked,
   - otherwise unify the parameter types with the argument types by walking
     `TypeKind` and `TypeExprId`,
   - missing concrete type reports `E3011`.
5. Key instances by `(decl id, type-argument tuple)` and intern exactly one
   instantiated declaration per key.
6. Substitute through the signature, body and nested generic uses. `Self` maps to
   the instantiated owner.
7. Recursion is bounded by an arena-lived step counter; reaching the bound emits
   `E3012`.
8. Mangling extends:

   ```text
   <module>.<Owner>.<name>(<params>)
   ```

   by appending `<type1,type2>` after the params string.

## Implementation Steps

1. Write the instantiation pass as a standalone module under `src/comptime/`.
2. Replace the HIR solver call in `compilation-session.cpp` with the pre-HIR pass.
3. Change `PerModuleSema` to accept a callback/supplier for instantiations so the
   generic call path no longer hard-reports `E3001`.
4. Add lowering of instantiated declarations in `HirLowerModern` when it encounters
   a concrete instantiation key.
5. Extend cache records for instantiated declarations and type-argument tuples;
   verify cold/warm builds with `--cache-stats`.
6. Add recursion traversal that visits nested uses before reporting errors.

## Diagnostics

Add:

- `E3009 ConstraintNotSatisfied` (reserved, enforced in step-05).
- `E3010 GenericArityMismatch`: explicit type-argument count differs.
- `E3011 CannotInferTypeArgument`: a type parameter cannot be inferred.
- `E3012 InstantiationRecursionLimit`: fixed product-of-depth limit reached.

In this step `E3009` is not emitted yet; leave it reserved.

## Verification

Add `tests/test-generics-mono.cpp`:

```zith
fn id<T>(v: T): T { return v; }
fn pair<T, U>(a: T, b: U): (T, U) { return (a, b); }
fn main(): i32 {
    let a = id<i32>(3);
    let b = id(4);
    return a + b;
}
```

Assert two instantiations of `id<i32>` collapse to one mangled symbol and the
program runs. Add negative cases for `E3010` and `E3012`.

Commands:

```bash
cmake --build build -j --target test-generics-mono
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `impl-status.md`: generic instantiation moves to Working.
- `docs/roadmap.md`: F-38 complete.
- `memory/nra-hir-boundary.md`: update the “current execution order” paragraph.
