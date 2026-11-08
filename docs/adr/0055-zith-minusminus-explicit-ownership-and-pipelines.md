# Zith-- Explicit Ownership and Pipelines

Status: accepted design, implementation pending

Zith-- keeps resource cleanup explicit and adds limited ownership transfer,
retained-reference, pipeline, type-kind, and variadic-macro rules. This ADR
records accepted language design, not an implementation order or a claim about
current compiler support.

## Context

The current pipeline spells the observing stage `do` and the current value
`..`. The SRA holder proposal rejects every unnamed owner temporary, including
one passed to a consuming function. The existing cleanup roadmap also proposes
a language-level `drop` hook. These choices do not match the desired Zith--
model, which makes ownership transfer explicit through consuming calls and
keeps cleanup under programmer control.

## Decision

### Ownership and cleanup

- Passing by value consumes ownership by default. A consuming callee receives
  the cleanup obligation and must explicitly clean, return, or transfer the
  owner on every function exit path.
- `retain` marks a non-owning reference that the callee may keep after the
  call. It does not transfer ownership. A temporary passed to `retain` is
  rejected. The initial analysis only enforces that temporary restriction and
  does not prove that a named owner outlives every retained use.
- An unnamed owner temporary may be passed to a consuming parameter. The
  callee then receives its cleanup obligation. An unnamed owner may not be
  discarded or passed to `retain`.
- Zith-- has no automatic cleanup and no language-level `drop` feature.
  Cleanup remains explicit.

### Pipelines

- `|>` is a consuming stage. It transfers the current value to the stage, and
  `it` must be consumed exactly once on every execution path. Passing `it` to a
  by-value parameter, returning it, or moving it into another owner counts as
  consumption. A new owner receives the cleanup obligation.
- A consuming stage may not pass `it` to a `retain` parameter.
- `also` is an observing stage. It borrows `it` read-only, may use it zero or
  more times, and leaves the chain value unchanged. The borrow cannot escape
  the stage. `it` may only be passed to read-only `view` parameters.
- Pipeline stages cannot be nested. Anonymous functions remain allowed, do
  not capture outer locals, and may contain independent pipelines.
- Zith-- reserves `also` and `it`. `also` replaces pipeline `do`, and `it`
  replaces the pipeline placeholder `..`. Other uses of `..` are unchanged.

### Type-kind predicates

- `if (T is struct)`, `component`, `enum`, and `union` are compile-time type
  predicates. Aliases resolve before the kind test, and only the selected
  branch is instantiated.
- These predicates do not expose fields or permit type mutation.

### Variadic cleanup macro

- A macro may declare one final variadic parameter, whose argument kind is
  declared using the existing macro argument kinds. The tail accepts zero or
  more arguments. Template repetition emits once per argument in input order.
- `@deferDestroy` accepts identifiers and registers one deferred cleanup body
  in the caller's lexical block. It calls each identifier's `.destroy()` in
  argument order. An empty argument list registers no cleanup.
- After `@deferDestroy(x)` is registered, `x` remains usable until block exit,
  but cannot be moved, rebound, or explicitly destroyed before the deferred
  call runs.
- `@deferDestroy` does not introduce automatic cleanup or a `drop` feature.

## Considered Options

- **Automatic owner cleanup.** Rejected. Zith-- leaves cleanup visible through
  explicit cleanup calls and `defer`.
- **Reject all owner temporaries.** Rejected. A consuming call has a named
  callee scope that can receive the cleanup obligation. Temporaries remain
  invalid for `retain`.
- **Count pipeline uses textually.** Rejected. `|>` consumption is checked
  per execution path, so mutually exclusive branches may each consume `it`.
- **General field reflection or type construction.** Rejected. The accepted
  type test only distinguishes the four declared type kinds.

## Consequences

- The holder heuristic in ADR-0050 is superseded. Its value-declaration lint
  remains accepted.
- The retained-reference contract requires the referent to remain valid while
  used. The first implementation is deliberately weaker and only rejects
  temporaries, so named-owner lifetime safety is not yet proven.
- Consumption checks must account for control-flow paths and cleanup
  obligations. This does not imply automatic cleanup.
- The accepted design does not set feature priority or implementation order.
  The current Zith-- specification and implementation status remain unchanged
  until each feature is implemented and verified.
