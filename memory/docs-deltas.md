# Documentation Deltas

`docs/deltas/` is an isolated playground for edits to a canonical document.
You draft the change there, compare it against the target section, and import
it once reviewed. It complements `docs/plans/` (new ideas that need promises
and tests) and `docs/adr/` (signed decisions); it is not a fourth source of
truth.

## The Contract

A delta is one markdown file with a header (`Status`, `Target`, `Owner`,
`Related`), change blocks keyed by `path#anchor`, and a rationale. Status is
`draft`, `review`, `imported`, or `dropped`. Active deltas live in
`docs/deltas/`; finished ones move to `docs/deltas/archive/`.

A delta records only the change, never a copy of the canonical text. Copying
the section would create a second source of truth that drifts, the same failure
the concept registry avoids.

## The Check

`scripts/check-deltas.py` validates the set. It fails on a missing or unknown
`Status`, a missing `Target`, an active delta under `archive/` (or the reverse),
or a `Target` that does not resolve to a real file and heading. `--show
docs/deltas/<file>.md` prints the target section so the draft and the canonical
text can be read side by side.

It is registered as the `deltas-consistency` CTest case and runs in CI next to
`check-docs-consistency.py` and `check-concepts.py`.

## Lifecycle

Draft (copy `TEMPLATE.md`) to Compare (`--show`) to Review to Import (apply the
blocks, move to `archive/`, set `Status: imported`). A rejected idea archives as
`dropped` with its reason. The workflow is described in `docs/deltas/README.md`.
