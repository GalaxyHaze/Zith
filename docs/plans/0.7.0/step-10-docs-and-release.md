# Step 10 — Documentation And Release Reconciliation

## Goal

After this step, `impl-status.md`, `roadmap.md`, spec status headers, and the
changelog accurately describe what 0.7.0 implements and what is deferred.

## Prerequisites

All steps in this directory must be done. This step must be run last.

## Baseline Facts

- `impl-status.md` still reports trait/interfaces working only at parse level,
  generics as Partial, comptime as Spec only, and the capability table in
  `docs/04-traits-interfaces.md` is outdated.
- `docs/roadmap.md` still describes waves up to 09 with F-29 as Spec only.
- `CHANGELOG.md` has no `0.7.0` section.

## Implementation Steps

1. Update `impl-status.md` for every feature changed by steps 01-09; remove
   duplicate statuses so each table row is accurate.
2. Update `docs/roadmap.md`:
   - add the waves 10-13 with IDs F-35 … F-39,
   - mark F-15, F-16, F-17, F-29 and F-38 as complete,
   - mark `dyn` and capability semantics as future/deferred,
   - remove references to old `docs/plans/` NRA guide if any.
3. Rewrite the capability table in `docs/04-traits-interfaces.md`:
   - remove `Copy`,
   - list `Arithmetic`, `Index`, `Iterator`, `Range`, `Functor`, `Error`,
     `Allocator`, `Generator`, `Share`, `Lent`, `Trust`, `Unique`, `Null`, `Fail`,
   - mark all as recognized and future-activated except the base machinery.
4. Add the `for`-is-iteration / `if`-is-membership note to `docs/03-type-system.md`
   and `docs/09-control-flow.md`; it exists in the design but is not on the spec
   page.
5. Update `docs/11-comptime.md` and `docs/14-polymorphism.md` status headers:
   comptime eval/introspection Working, `dyn` not implemented.
6. Add a `CHANGELOG.md` `0.7.0` section listing the feature areas.
7. Update `memory/comptime-generics-traits.md` if the implementation changed any
   locked decision.

## Verification

No new source tests. Manually verify the documentation statements by running the
step reproducers and reading the final `zithc check` output.

Commands:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Check for broken internal links with `rg -n "docs/plans/nra-hir-guide|no topic files"`.

## Docs To Update

This step is the documentation update.
