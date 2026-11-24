# Named Blocks And Scope-Marker Cleanup

Status: accepted design, implemented

Zith-- adds two statement forms: a plain named block (`setup: { ... }`) and a
named block with a scope marker (`render: @scoped(a, b) { ... }`). This ADR
records the accepted design and the one deliberate tension with
[ADR-0055](0055-zith-minusminus-explicit-ownership-and-pipelines.md), which
states that Zith-- has no automatic cleanup.

## Context

A long `fn` or `state` body is today an anonymous sequence of statements. A
reader cannot see the phases of the function (setup, render, teardown) without
extracting helper functions, and a writer has no way to attach managed cleanup
to one phase. The alternative is to repeat `enter`/`leave` by hand, or to
register one `defer` per resource and take responsibility for the release
order and the hygiene of the guards. The common failure is to miss a `leave`
on one exit path or to release paired resources in the wrong order.

## Decision

### Statement forms

- A plain named block is written `name: { ... }`. It creates an ordinary
  lexical scope. The name has no runtime effect and serves reading, tooling,
  and diagnostics.
- A named block with a marker is written `name: @macro(args) { ... }`. The
  marker must name a macro declared with the `#[scope]` attribute.
- Position: both forms are valid only as a direct statement of a `fn` or
  `state` body. They are rejected inside `if`, loops, anonymous blocks, and
  other named blocks. The one composed position is a pipeline stage.
- A pipeline stage may be a named block, plain or marked
  (`left |> step1: { ... } |> step2: @scoped(a) { ... }`). The stage keeps
  every pipeline rule: it consumes or observes `it`, and control flow stays
  local to the stage.

### Marker semantics

- Each marker argument must be a place-expression: a binding, a parameter, a
  global, or a field. A temporary is rejected.
- Each argument's type must implement the `Scope` trait.
- The compiler expands the marker into one hidden `let guard = arg.enter();`
  plus a matching `defer guard.leave();` per argument. `enter` runs left to
  right over the arguments. `leave` runs in reverse through the existing
  `defer` infrastructure, so it runs on fallthrough, `return`, and any labeled
  exit that leaves the block.
- The guard binding is hidden and hygienic: the generated name cannot collide
  with a user identifier.
- `enter` and `leave` do not fail at the language surface. A failing
  implementation records the failure in its own stream.

### Names

- A named block name is descriptive and is never a `break`/`continue` target.
- Names are unique per `fn`/`state` body. Duplication is an error, including a
  collision with a loop label in the same body.
- Names are preserved in the HIR for tooling and diagnostics, like a loop
  label. They are not serialized into ZIRL and have no ABI effect, so a block
  can be renamed without changing a compiled artifact.

### Trait and macro

- The stdlib declares `trait Scope { fn enter(var self): dyn Guard; }` and
  `trait Guard { fn leave(var self): void; }`. Each implementation chooses its
  own guard type and the effect `leave` performs, such as a render checkpoint
  or a log flush.
- The scope marker is an ordinary stdlib macro, `scoped`, marked with
  `#[scope]`. The compiler dispatches on the attribute, not on a macro name,
  so no macro name is hardcoded. Only a `#[scope]` macro may appear in the
  outer position `name: @macro(...) { }`.
- The spec names the macro `marker`, but `marker` is a reserved keyword in
  Zith-- (the removed `flow fn`/`marker` model still reserves it). The stdlib
  macro is therefore named `scoped`. The attribute is the contract, so the
  name is not part of the design.

### Diagnostics

Three dedicated codes cover the new rejections: a named block in an invalid
position, a marker argument that is not a place or does not implement `Scope`,
and a duplicate name. A rejected marker argument suppresses the generated
`enter`/`leave` scaffolding so the dedicated diagnostic is not followed by
cascading errors from the generated calls.

## Tension With ADR-0055

ADR-0055 records that Zith-- has no automatic cleanup and no language-level
`drop`, and that cleanup stays explicit through `defer`. The scope marker keeps
that letter: the marker is a stdlib macro that emits `defer`, and the compiler
does not acquire an automatic owner-cleanup feature. What changes is
ergonomics. The author writes one marker instead of one `enter`/`leave` pair
per resource, and the compiler derives the release order from the argument
order. The cleanup is still explicit in the desugared form, still runs through
`defer`, and still fires only on block exit. No owner gains an implicit
`drop`. This ADR records the tension rather than overriding ADR-0055 in
silence.

## Considered Options

- **A dedicated HIR node with synthesized cleanup.** Rejected. Rewriting the
  named block after macro expansion to `let` plus `defer` reuses the existing
  method-call and defer machinery, so the runtime behavior is already proven
  and the marker stays a macro rather than a compiler feature.
- **A hardcoded `marker` macro name in the compiler.** Rejected. The `#[scope]`
  attribute is the contract, so the stdlib can rename or add markers without a
  compiler change.
- **Allowing named blocks anywhere a block is allowed.** Rejected. The
  position rule keeps the names at the phase level of a function instead of
  turning them into a general nested-structuring mechanism.
- **`break`/`continue` targeting a named block.** Rejected. Names are
  descriptive. Loop labels remain the only control-flow targets, and the two
  share one name space per body so an identifier has one meaning.

## Consequences

- A plain named block is free at runtime: it lowers to ordinary control flow.
- The marker form adds no ownership rule beyond what `defer` and method calls
  already provide. It does not introduce automatic cleanup or a `drop` hook.
- Tooling can list the phases of a function from the HIR name side table
  without reading the source.
- The dedicated diagnostics give one clear message per misuse instead of a
  cascade from generated code.
