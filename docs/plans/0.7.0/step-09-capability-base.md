# Step 09 — Capability Base Only

## Goal

After this step, capability names are reserved, `implement T as <Capability>` is
shape-validated, and the compiler has a clear extension point for future
capability behavior. No capability changes compilation behavior in 0.7.0.

## Prerequisites

- step-03 (conformance and method signature checks).

## Baseline Facts

- `docs/04-traits-interfaces.md` currently lists `Copy`, `Functor`, `Arithmetic`,
  `Error`, `Null`, `Fail`, `Allocator`, `Generator`, `Share`, `Lent`, `Trust`,
  and `Unique`.
- 0.7.0 removes `Copy` from that list; implicit bitwise copy is a type-system
  property, not a capability.

## Design Contract

1. Add `src/sema/capabilities.hpp/cpp` with:

   ```cpp
   struct CapabilitySpec {
       std::string_view name;
       memory::DynArray<CapabilityMethod> methods;
       enum class Behavior : uint8_t { None, Arithmetic, Index, Iterator, Range, Null, Fail };
       Behavior behavior = Behavior::None;
   };

   const CapabilitySpec *lookupCapability(std::string_view name);
   bool isCapabilityName(std::string_view name);
   ```

2. Register exactly: `Arithmetic`, `Index`, `Iterator`, `Range`, `Functor`,
   `Error`, `Allocator`, `Generator`, `Share`, `Lent`, `Trust`, `Unique`, `Null`,
   `Fail`. `Behavior::None` for all of them.
3. `Arithmetic` requires the binary operator signatures for `+ - * / %`;
   `Index` requires `operator [](self, index) : T`; `Iterator` requires
   `next(self)` returning an iteration state and `done(self)`/`value(self)` or an
   equivalent fixed shape recorded in the registry; `Range` requires
   `contains(self, value) : bool`. Exact shapes are written explicitly in the
   registry implementation and must be copied by future capability activations.
4. Capability names cannot be used as user trait names; duplicate/well-formedness
   checks report `E2028`/`E2029`.
5. `implement T as <Capability>` validates the method set against the spec and
   permits only the documented methods. Same-shape checks reuse the step-03
   signature comparison.
6. When a capability is used in a trait-typed context or method dispatch, the
   compiler reports "capability `X` recognized but not yet enforced" with
   `E2029`, not a generic E2007.
7. `Null` and `Fail` are registered with `Behavior::Null` / `Behavior::Fail` but
   are blocked on NRA proof; no semantic activation in 0.7.0.

## Implementation Steps

1. Add the registry and `CapabilitySpec` types.
2. Wire `implement T as <Capability>` into sema’s impl checking.
3. Add reserved-name diagnostics.
4. Document the extension recipe in this file (the exact wire site for each
   future behavior):
   - `Arithmetic`: binary operator resolution in `PerModuleSema::typeOf` /
     overload selection,
   - `Index`: postfix `a[i]` resolution before the existing non-indexable error,
   - `Iterator`: `for (x in xs)` desugaring in sema/HIR lowering,
   - `Range`: `in` in condition position in parser/sema, distinct from iterator
     `for` semantics.
5. Update the spec capability table.

## Diagnostics

Add:

- `E2028 CapabilityShapeMismatch`.
- `E2029 CapabilityNotImplemented`.

## Verification

Add `tests/test-capabilities.cpp`:

```zith
implement Vec3 as Arithmetic {
    fn +(self, other: Vec3): Vec3 { return self; }
}
```

Because `Arithmetic` is not active, this must pass shape validation but any use of
`+` on `Vec3` must report `E2029` (recognized, not enforced).

Negative:

```zith
implement Vec3 as Arithmetic {
    fn wat(self): i32 { return 0; }
}
```

Must report `E2028`.

Commands:

```bash
cmake --build build -j --target test-capabilities
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `docs/04-traits-interfaces.md`: rewrite capability section to mark base
  infrastructure and remove `Copy`.
- `impl-status.md`: capability registry/signature checking Working; capability
  semantics Spec only/deferred.
- `docs/roadmap.md`: F-39 complete, with activation steps marked future.
