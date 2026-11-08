# SRA Holder and Ephemeral-Owner Heuristics

Status: accepted, holder heuristic superseded by ADR-0055

The `Zith--` `main` keeps SRA (Small Resource Analysis) as its small, frozen
ownership analysis. It does not grow into the full NRA proof machine and does
not introduce automatic `drop`. This ADR adds two heuristics to SRA and records
the boundary between what SRA checks and what NRA must still prove.

## Context

The existing `Zith--` ownership checks are the `lend`/`view` call-annotation
slice, deterministic use-after-move, and `E4008 PointerEscapesScope`. `E4008`
is a pointer-lifetime check in sema, not one of the facts that the SRA section
of `docs/Zith---implementation.md` lists. There is no cleanup obligation
tracking, so a resource owner that is created as an unnamed
temporary cannot be cleaned: nothing names it, so no `defer` or `destroy` call
can reach it, and any non-owning handle stored from it (a reference, a `view`/
`lend`, or a `dyn Allocator` handle kept inside a collection) is left pending
when the temporary's storage dies. The concrete case is `createArray(Arena{50})`,
where the arena is an ephemeral allocator whose storage ends with the
instruction but whose handle the array retains.

## Decision

SRA gains two heuristics in this decision. ADR-0055 supersedes the holder
heuristic and its strict rejection of every unnamed owner temporary. The
value-declaration lint remains unchanged.

1. **Holder heuristic (safety).** A value that carries a cleanup obligation must
   be bound to a name with a scope, so a `defer`/`destroy` path can run against
   it. An owner that appears only as an unnamed temporary is rejected. This is
   the strict form: it does not analyze whether the callee retains the value, so
   `consume(Buffer{...})` is rejected even when `consume` consumes it. The rule
   is stated over the cleanup obligation, not over the declaration keyword, so a
   `struct` with only primitive fields that wraps a global resource is still an
   owner and still protected.

2. **Value-declaration lint (style).** A `struct` whose fields are all primitive
   (or components) derives no cleanup obligation from its fields, and warns that
   it should be written as `component`. This is a warning, not an error, because
   a primitive-only `struct` is legal and may still be an owner through methods
   or global state. It is a declaration lint and is independent of the holder
   heuristic, which is a use-site rule.

The `component`/`struct` split is intended to be sound as a value/object
distinction because `component` is restricted by construction: it may hold only
primitives, integer-backed enums, other components, and arrays or slices of
those. The sema enforces this restriction for local declarations and substituted
generic fields, including imported generic components. It rejects struct,
union, pointer, optional, and other non-value fields, along with base-type
declarations and explicit trait implementations. This enforcement is required
for the soundness claim, rather than an assumption.

Cleanup stays explicit. A held owner is released through `defer` (or a direct
`destroy` call), never through an implicit drop hook. The holder heuristic exists
only to guarantee that the owner is reachable by such a path.

## Considered Options

- **Error for the declaration lint.** Rejected: a primitive-only `struct` is
  valid and can be an owner, so the keyword is a style signal, not the
  definition of "object".
- **Retention-aware holder heuristic.** Deferred: it needs a retention signal on
  the callee (a `retain`-style parameter or an effect header). The strict form
  needs neither, and a future retention signal can relax it later. The candidate
  spelling is `retain`, kept distinct from the full-Zith `Bind (^)` qualifier.
- **Grow SRA into the NRA proof.** Rejected: the full ownership proof is
  full-Zith work, specified in `docs/nra-spec.md`.

## Consequences

- SRA stays small and conservative: it can reject correct code (the unnamed
  temporary passed to a consuming callee) rather than prove it correct.
- The two heuristics do not catch a named owner that dies before the handle it
  produced. That is the retention case and belongs to NRA or to a future
  retention-aware extension of SRA.
- The rule is general. It is not allocator-specific, so no allocator vocabulary
  is added to the glossary.
- The `struct`/`component` convention becomes visible in diagnostics without
  changing the meaning of either declaration.
- The holder heuristic needs a "carries a cleanup obligation" predicate over a
  type. The declared owner markers today are the legacy `destroy` method and
  `InPlace` (see `docs/adr/0010-allocator-inplace-drop.md`). Neither is wired
  into sema yet, so that predicate is part of the implementation work, not a
  given.
