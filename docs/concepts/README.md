# Concept Registry

This directory consolidates the concepts of the Zith language (the sibling
language described by [Zith-spec.md](../Zith-spec.md)), not `Zith--`.

The problem it solves: the Zith surface is enumerated flat in the spec
appendix (keywords, intrinsics, attributes) and decided across many ADRs and
sibling analysis specs, but nothing ties a surface form to the one document
that owns its meaning. A concept can exist in three places (spec appendix,
chapter doc, ADR) with no declared authority, and a detail is then easy to
forget.

## What the registry is

One table entry per **concept**, not per spelling. Several spellings may share
one concept (`@struct` / `@component` / `@union` / `@enum` are one type-kind
predicate; `&T` / `&mut T` / `^T` / `%T` are one ownership concept). Each entry
declares:

- `Concept`: a short human name for the idea.
- `Spellings`: every surface form the spec appendix lists for it.
- `Authority`: the single document and section that owns the meaning, written
  as `path#section`. The checker rejects a citation that does not resolve.
- `Decision`: the ADR that fixed the spelling or semantics, or `-`.
- `Sibling`: an analysis spec (NRA, MRA, RRA, NIA, ZPK) that constrains it, or
  `-`.
- `Status`: `prose` when a chapter section defines it, `appendix` when only the
  spec appendix table defines it, `adr` when only an ADR decides it.

## What the registry is not

It is not a copy of the spec. It points at the authority, it does not restate
the definition. Restating would create a fourth source of truth that drifts.

## The check

`scripts/check-concepts.py` reads the spec appendix and the registry and fails
when they disagree:

1. Every surface form in the appendix table must appear in the registry.
2. Every spelling in the registry must appear in the appendix table.
3. Every `Authority` citation must resolve to a real file and section.

Run it from the repository root:

```bash
python3 scripts/check-concepts.py
```

It also prints the count of `appendix` entries. That count is the consolidation
gap: the number of concepts that only the appendix table defines and that have
no prose authority yet. It is informational, not a failure.

## Current scope

This prototype covers the compiler intrinsics (spec appendix A.2). Keywords and
operators (A.1) and attributes (A.3) are the next slices.
