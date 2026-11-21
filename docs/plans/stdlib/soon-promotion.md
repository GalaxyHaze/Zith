# Standard Library `soon`-First Promotion

Status: drawing

New standard-library modules start under `soon/` and move to `std/` only after
they clear a stability gate. The repository documents `soon` as experimental in
`docs/20-standard-library.md` and `CONTEXT.md`, but it has no written rule for
the gate itself. Without one, a module can drift between the two roots, and a
provisional API can be treated as stable by accident.

This drawing fixes the promotion policy and inventories the current `soon/`
surface. It is the framing plan for the per-module drawings that follow.

Size: system, the standard-library delivery surface.

## Current `soon/` Surface

| Module | Path | State | Domain |
|---|---|---|---|
| string | `stdlib/soon/string.zithm` | implemented, type-checked, no signed promises | owning byte string |
| json | `stdlib/soon/json.zithm` | stub | parse, stringify, isValid |
| result/fallback | `stdlib/soon/result/fallback.zithm` | implemented, native-tested, VM v2 and promise tests pending | `Result` fallback access |
| math/scalar | `stdlib/soon/math/scalar.zithm` | implemented, native-tested, VM v2 and promise tests pending | integer bounds and angle conversion |
| algorithms/slice | `stdlib/soon/algorithms/slice.zithm` | implemented, native-tested, VM v2 and promise tests pending | `i32` slice search and counting |

`test-stdlib-modules` type-checks every `stdlib/**.zithm` in isolation.
The `string` module still lacks the per-promise contract tests the gate below
requires. The Result, math, and slice helpers have native runtime coverage, but
they are not ready for promotion until they also meet the full gate.

The stable `std/` surface is not in scope here. The allocator-ownership and
collections drawings under `docs/plans/stdlib/` cover the `std/` side.

## Promotion Gate

A module is eligible to move from `soon/` to `std/` when all of the following
hold:

1. The module has no open decisions in its drawing.
2. Every public symbol has a contract test that carries a promise id.
3. The module compiles and runs on the native and VM v2 backends.
4. The module has not changed its public surface for the agreed stabilization
   window.

The exact window is an open decision below. "Ten percent stable" is the working
intuition, not a number this drawing fixes.

## Non-goals

- Does not change compiler behavior.
- Does not move any existing module.
- Does not define the stable `std` ABI or its layout obligations.
- Does not add `soon/` to the default include path.

## Open decisions

- What is the measurable stabilization window, and is it time-based or
  change-based?
- Does promotion rename the import path, and how are existing `soon/` users
  handled?
- Where does the gate live: an ADR, `20-standard-library.md`, or one plan per
  module?
- Does `soon/` expose a single facade module, or only per-module imports?
- Which module is promoted first, and what is the reference example of a full
  draw to conform cycle?
