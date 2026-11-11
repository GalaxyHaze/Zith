# Documentation Deltas

A delta is an isolated playground for a change to a canonical document. You
draft the change here without touching the document, compare the draft against
the target section, and, once it is reviewed, import the change into the
document and archive the delta. This directory holds the active drafts. The
finished ones move under `archive/`.

## Why A Delta Is Not A Copy

A delta records only the change, never a copy of the canonical text. Copying
the section would create a second source of truth that drifts from the first,
which is the same failure the concept registry avoids (see
[docs/concepts/README.md](../concepts/README.md)). A delta is a patch plus the
reason for it, not a rewrite of the document.

## When To Use A Delta

Use a delta when the work is an edit to text that already exists: a spec
section, a chapter doc, or an ADR. Use [docs/plans/](../plans/) when the work
is a new idea that needs promises, a seam, and conforming tests, and use
[docs/adr/](../adr/) when the change is a decision that must be signed.

The three are complementary. A delta can be the drawing that later becomes a
plan, and a change that is hard to reverse still ends in an ADR. The delta is
the scratch surface, not a fourth authority.

## The Delta File

Each delta is one markdown file under `docs/deltas/`. It starts with a header,
then the change blocks, then the rationale.

```
Status: draft
Target: docs/Zith--.md#globals
Owner: <name>
Related: docs/adr/00NN-<decision>.md
```

The header fields:

- `Status`: `draft`, `review`, `imported`, or `dropped`.
- `Target`: the authority the change applies to, written as `path#anchor` where
  `anchor` is the GitHub heading slug. A `dropped` delta may use `-`.
- `Owner`: the person who owns the delta.
- `Related`: a plan or ADR that motivates it, or `-`.

The change blocks come next. One block per target section, each keyed by the
same `path#anchor` form, with `add`, `change`, and `remove` lines. The rationale
closes the file: why the change is needed, in short prose.

## The Lifecycle

1. **Draft**: copy [TEMPLATE.md](TEMPLATE.md) to `docs/deltas/<topic>.md`, set
   `Status: draft`, and write the change blocks.
2. **Compare**: run `python3 scripts/check-deltas.py --show docs/deltas/<topic>.md`
   to print the canonical section next to the draft. Keep the delta and the
   document aligned by hand while the change is open.
3. **Review**: set `Status: review` when the draft is ready for a reader.
4. **Import**: apply the change blocks to the target document, then move the
   file to `docs/deltas/archive/` and set `Status: imported`.

If the idea is rejected, move the file to `archive/` and set `Status: dropped`.
An archived delta keeps the reason, so a later reader does not reopen a settled
decision without context.

## The Check

`scripts/check-deltas.py` enforces the contract. It fails when a delta has no
`Status` or `Target`, when the status is unknown, when an active delta is not
under `docs/deltas/` or an archived one is not under `archive/`, or when a
`Target` does not resolve to a real file and heading.

Run it from the repository root:

```bash
python3 scripts/check-deltas.py
```

It is registered as the `deltas-consistency` CTest case and runs in CI next to
the docs and concept checks.
