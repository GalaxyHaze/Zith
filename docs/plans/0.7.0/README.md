# Zith 0.7.0 Implementation Steps

This directory contains the execution plan for Zith 0.7.0: **Comptime, Generics,
Traits, and the Capability Base**. Each file is a self-contained guide for a
separate agent session. A session should read this README and its own step file,
then implement only that step, verify it, and stop.

The durable memory version of this contract is
`memory/comptime-generics-traits.md`; the step files are the source of truth for
implementation detail.

## Feature IDs

| ID | Feature | Step |
|---|---|---|
| F-35 | Trait and interface bodies become real declarations | step-01 |
| F-36 | `requires` and `extends` become parsed, stored constraints | step-02 |
| F-37 | Trait/interface conformance and structural interface satisfaction | step-03 |
| F-38 | Generic monomorphization before HIR | step-04 |
| F-29 | Generic constraint enforcement (`T: Trait`, `+` bounds) | step-05 |
| F-15/F-16 | `const { }` and `const fn` compile-time evaluation | step-06 |
| F-17 | Static introspection queries and `@fields` iteration | step-07 |
| F-17 | Type construction with `@struct` / field / method mutation | step-08 |
| F-39 | Capability registry base and extension recipe | step-09 |
| - | Documentation and release reconciliation | step-10 |

## Dependency Order

Dependencies are strict only where stated. A step may be started once its
prerequisites are complete, even if later steps are not.

```text
step-01 (trait bodies)
  -> step-02 (requires/extends)
  -> step-03 (conformance)
  -> step-05 (constraints)
  -> step-06 (CTFE)
     -> step-07 (introspection)
     -> step-08 (type construction)
step-04 (monomorphization) -> step-05
step-03 -> step-09 (capability base)
```

## Shared Rules

- Do not modify implementation files outside the step being implemented.
- Every step must end with the project building and its focused tests passing.
- Run `cmake --build build -j` and the step's focused target before CTest.
- Update `impl-status.md` only for behavior changed by the current step.
- Use the feature IDs in commit messages and test names.
- Follow `.clang-format`, the existing arena/`DynArray` ownership, and no-exceptions /
  no-RTTI style.
- Do not revert unrelated user worktree changes seen at the start of a session.

## Diagnostic Reservation

Parallel sessions must use only the codes owned by their step. Do not reuse codes
outside these ranges without updating this README and `src/diagnostics/error-codes.hpp`.

| Range | Kind | Owner |
|---|---|---|
| 2021-2029 | Trait/interface/capability | steps 01-03 and 09 |
| 3009-3012 | Generic instantiation and constraints | steps 04-05 |
| 6001-6006 | Comptime evaluation and type construction | steps 06-08 |

Exact constants per step are listed in the step file.

## Definition Of Done

1. The step's reproducer changes from the captured current diagnostic to the
   expected new behavior.
2. The new CTest executable is registered with `add_zith_test` in the root
   `CMakeLists.txt` and passes.
3. `cmake --build build -j && ctest --test-dir build --output-on-failure` has no
   failures introduced by the step.
4. Formatter round-trip passes for the new syntax (`cmake --build build --target
   fmt-check` on touched files, or `zithc check` proves the round trip).
5. The step's `Docs To Update` section is actually applied.

## Capability Base Note

0.7.0 ships capabilities as infrastructure only. `Arithmetic`, `Iterator`,
`Range`, and `Index` are not activated in this release; step-09 documents the
recipe to activate them later. `Copy` is not a capability. `Null` and `Fail`
are registered but blocked on NRA proof.
