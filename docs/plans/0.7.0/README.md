# Zith 0.7.0 Zith-- Planning

> `Zith--` is the sibling language built and maintained by `main`. This plan
> tracks `Zith--`; Zith has a separate specification and development track.
> Comptime and the capability model described for Zith are not `Zith--` targets.

## Scope

This directory tracks current `Zith--` planning and retired steps:

- The accepted ownership, pipeline, type-kind, and variadic cleanup design is
  not yet assigned an implementation order. See
  [ADR-0055](../../adr/0055-zith-minusminus-explicit-ownership-and-pipelines.md)
  and [the variadic macro plan](../variadic-macros-and-defer.md).
- Automatic `drop` cleanup is rejected for Zith--. See the historical
  [`defer-drop.md`](../defer-drop.md) proposal.
- Platform-specific imports (`foo.<arch>.<os>.zith`) are implemented in the
  import resolver and archived. See
  `docs/plans/archive/platform-imports.old.md`.
- Monolith splits as infrastructure work. See `docs/plans/monolith-splits.md`.
- Feature hardening and debt closure from `docs/implementation-debt.md`.

Traits, generics, conformance and generic constraints are already implemented
in `Zith--` and documented under `docs/impl-status.md`.

## Related Plan Locations

- `docs/plans/defer-drop.md`: historical `drop` proposal, superseded by
  ADR-0055.
- `docs/plans/monolith-splits.md`: source reorganization priority and contract.
- `docs/plans/archive/0.7.0-zith/`: the older 0.7.0 full-Zith planning for
  comptime, introspection, type construction, capabilities and NRA steps.
- `docs/plans/standalone-c-toolchain.md` and `docs/plans/tiny-c-backend.md`:
  native toolchain roadmap.

The older full-Zith plan is intentionally archived. Comptime evaluation,
reflection, type mutation and activated capabilities remain Zith (full-spec)
features, not `Zith--` candidates.

## Retired Steps

- `docs/plans/archive/platform-imports.old.md`: platform imports
  (`foo.<arch>.<os>.zith`) are implemented in Zith--; no active
  implementation steps remain.
- `docs/plans/archive/parse-input-cast.old.md`: `ParseInput` and
  `InputLine.cast<T>` are shipped; no active implementation steps remain.

Active plans must not repeat implemented wave work. Status lives in
`docs/impl-status.md`; roadmap statuses and debt annotations point back to it.
