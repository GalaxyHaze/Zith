# HirMakeDyn Addressability Contract

## Status

Accepted.

## Context

HirMakeDyn builds a two-word fat pointer of data and vtable for a value erased
to `dyn Trait`. The dyn data slot must hold the address of the concrete value so
a trait method with a `lend self` receiver can mutate the original.

`CodeGenEmit::emitMakeDyn` in `src/codegen/codegen-emit-expr.cpp` currently
cannot tell whether the incoming `HirExprId` is already a stable place. For
aggregates it unconditionally allocates a new slot, stores the value there, and
points the data slot at that spill. A spilled copy breaks a mutating receiver:
the callee writes to the copy and the caller never observes the mutation. This
is the root of the `lend dyn` receiver debt, section 12 of
`docs/implementation-debt.md`.

The lowering in `src/sema/hir-lower-expr-value.cpp` already distinguishes the
cases by hand. Slices and primitives are spilled explicitly and their
`source_type` marks the shape, and aggregates pass the loaded value. But
`HirMakeDyn` carries no flag that tells the codegen whether `value` is a place
address or a loaded value, so the codegen re-spills to be safe.

## Decision

`HirMakeDyn` gains an explicit `value_is_place` flag. When the flag is set, the
lowering emits the address of the original place and the codegen uses it as the
dyn data pointer without allocating a new slot. When the flag is clear, the
codegen keeps the current spill behavior for register values and temporaries.

Addressability becomes a contract between sema and codegen instead of a codegen
guess.

`HirMakeDyn` is serialized into the `.zirl` cache and the WASM flat HIR blob, so
the flag must survive the round-trip. The native cache stores it in the existing
`CompactExpr::flags` byte, which already round-trips and needs no format-version
bump. The WASM blob adds a flag byte after the vtable name and bumps
`kBlobVersion` from 1 to 2, because that encoding has no spare byte and older
readers would misparse the new field.

## Considered Options

- Make the lowering always pass a slot for aggregates. Rejected because it
  forces a spill even when a stable place already exists, which cancels the
  mutation fix it is meant to enable.
- Let the codegen detect a place heuristically through `alloca` or GEP
  patterns. Rejected because it is not verifiable and breaks silently on any
  lowering change.

## Consequences

- The `dyn lend` receiver fix has a defined seam: the addressability flag in
  HIR and its use in `emitMakeDyn`.
- A second slice accepts `self: lend dyn Trait` in sema. `registerNamedTypes`
  interns the real trait entry before method signatures lower,
  `checkImplementBlocks` treats a `*dyn Trait` requirement as a receiver slot,
  and the vtable adapter rebuilds the fat pointer when the implementation
  spells the receiver as `dyn Trait` too.
- Migrating `stdlib/std/io/format.zithm` from `lend FormatBuffer` to
  `lend dyn TextSink` is a separate follow-up once the receiver works.
