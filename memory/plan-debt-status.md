# Plan, Debt and Status Curation

This note records the contract used when curating Zith-- plans, the
implementation-debt inventory, and the status document. It also records the
relationships that are easy to get wrong when editing these files.

The authoritative status is `docs/impl-status.md`; plans and debt annotations
must cite it instead of restating a guessed status. `docs/roadmap.md` and the
active directories under `docs/plans/` are planning surfaces.
`docs/implementation-debt.md` is the engineering inventory for gaps and
repeated patterns.

This note exists because plan curation is not the same as feature
implementation. A plan may contain an old wave description that looks
future-facing even though the feature row in the status document says
`Working`. A debt entry may also look like an open task when it actually
records an intentional product boundary. The curator's job is to keep those
three surfaces mutually consistent without silently changing compiler
behavior.

Language semantics stay defined by `docs/Zith--.md` and
`docs/impl-status.md`. Memory notes should keep short decision records and
point at those files instead of restating full specifications.

## Current Plan Inventory

The active plan homes are:

- `docs/plans/0.7.0/`: current Zith-- iteration scope and retired-step index.
- `docs/plans/defer-drop.md`: historical `drop` proposal, rejected by ADR-0055.
- `docs/plans/monolith-splits.md`: source-reorganization order.
- `docs/plans/archive/platform-imports.old.md`: archived implementation
  contract for platform imports.
- `docs/roadmap.md`: stable feature IDs and wave narrative.
- `docs/plans/standalone-c-toolchain.md` / `docs/plans/tiny-c-backend.md`:
  active Zith infrastructure work for the native C toolchain.
- `docs/plans/codebase-quality-remediation.md`: parser-depth hardening and
  ASan/UBSan CI steps based on
  `docs/codebase-audit-2026-10-15.md`. The completed 2026-10-07 remediation
  record is archived at
  `docs/plans/archive/codebase-quality-remediation-2026-10-07.old.md`.
- `docs/plans/idea-lifecycle-small-scope.md` / `docs/specs/abi-lifecycle.md`:
  small-scope lifecycle setup for ideas, ABI promises, and conforming tests.
- `docs/plans/forward-dispatch.md`: Zith-- call-site overload selection from a
  union or `opaque` tag, drawing stage. The enum level is deferred to a
  separate full-Zith unit.
- `docs/plans/variadic-macros-and-defer.md`: variadic macro statement
  repetition and caller-scope cleanup registration, drawing stage.
- `docs/plans/macro-type-aware-branches.md`: type-aware macro arguments,
  generic type-category predicates, and `@staticAssert`, drawing stage.
- `docs/plans/abi/explicit-alignment.md`: Zith-- type and binding alignment,
  currently promising and pending idea-owner review.
- `docs/plans/stdlib/allocator-ownership.md`: explicit allocator ownership and
  migration across stdlib allocations.
- `docs/plans/stdlib/dynarray-result-panic.md`: `DynArray`-focused collection
  and error-handling expansion, dependent on allocator ownership.
- `docs/plans/stdlib/soon-promotion.md`: `soon`-first delivery policy and the
  promotion gate from `soon/` to `std/`.
- `docs/plans/stdlib/soon-string.md`: owning byte-string contract for
  `stdlib/soon/string.zith`.
- `docs/plans/stdlib/soon-json.md`: JSON value model and text boundary for the
  `stdlib/soon/json.zith` stub.
- `docs/plans/stdlib/soon-random.md`: random generator and uniform-distribution
  surface for a proposed `soon/random` module, drawing stage.
- `docs/plans/stdlib/soon-io.md`: executor, task, and non-blocking I/O surface
  for a proposed `soon/io` module, drawing stage.
- `docs/plans/stdlib/soon-thread.md`: thread library for Zith-- with a
  `ThreadBackend` trait, `Job` blueprints, `ThreadHandle<T>`, and runtime
  `Share`/`Revokable` wrappers, drawing stage. It is ordinary runtime code and
  does not add `fork`/`merge`/`revoke`/`spawn` keywords.

The archived plan homes are:

- `docs/plans/archive/0.7.0-zith/`: full-Zith comptime, introspection, type
  construction, capabilities, and NRA planning.
- `docs/plans/archive/release-artifacts.old.md` and
  `docs/plans/archive/release-stdlib.old.md`: release/installer audit
  snapshots, not active feature plans.
- `docs/plans/archive/traits-interfaces-*.old.md`: prior trait/interface step
  notes.
- `docs/plans/archive/parse-input-cast.old.md`: shipped `ParseInput` step.
- `docs/plans/archive/platform-imports.old.md`: shipped platform-import step.
- `docs/plans/bootstrap-slices-fnptr.md`: shipped bootstrap step; kept as a
  maintenance note, not an active plan.
- `docs/plans/branch-protocol.md`: full-Zith design for `Branch`/`fork`/`merge`;
  not a Zith-- deliverable.

Completed active steps should move to `docs/plans/archive/`. The retired-step
index under `docs/plans/0.7.0/README.md` should name the archived file so
future curators do not resurrect the plan by accident.

## Feature Status Rules

`Working` in `impl-status.md` means the feature is accepted by parser and sema
and lowers through HIR to LLVM codegen. `Parse skipped`, `Spec only`,
`Parse-level in progress`, and `Stub` are real non-working statuses and must
not be overwritten in plans unless the implementation in `impl-status.md`
changed first.

Several roadmap rows looked stale because the feature table still described
old waves. For example `when`/`match`, `for` variants,
`state`/`dock`/`jump`, and tagged-union or opaque `is <type>` narrowing are
`Working` in the status document. The roadmap now records the current status
and points Wave 02/Wave 03 text to `impl-status.md` instead of repeating an
old acceptance checklist.

The roadmap still uses feature IDs F-01 through F-42. The row-level status is
a short hand for what is current. The Wave narrative is a forward-looking
explanation. They can disagree when a wave was partially completed, so both
must be checked before editing. A row marked `Working` cannot remain described
as pending in the same wave section without an explicit reason such as
full-Zith remainder.

## Completed Since Last Audit

Platform imports (F-42) shipped and the roadmap row is `Working`. The active
plan was moved to `docs/plans/archive/platform-imports.old.md` and the 0.7.0
README now lists it under Retired Steps.

C header imports progressed from the historical `Working (common C)` wording
to `Working (validated C)`. Simple records passed/returned by value are
imported only after libclang proves layout/alignment for the configured target; scalar,
pointer, and nested validated-record fields are supported. Unverified records
are skipped, and function-like macros, strings, globals, bitfields,
packed/anonymous records, flexible arrays, `long double`, and `__int128`
remain unimported.

The `src/session/frontend-context.cpp` and `src/session/compilation-session.cpp`
monolith splits are merged, and the `src/codegen/codegen-emit.cpp` monolith is
also split. `docs/plans/monolith-splits.md` now records the completed
translation units; `hir-lower-expr.cpp` and `frontend-expr.cpp` remain
secondary candidates.

## Status Map Used In This Audit

The map below records the statuses consulted from `docs/impl-status.md`. It is
not a substitute for the status document; it is a durable decision record for
why specific roadmap rows were updated.

| ID | Roadmap status after curation |
|---|---|
| F-01 | Working |
| F-02 | Working, synonym for `when` |
| F-03 | Working |
| F-04 | Working |
| F-05 | Parse-level in progress |
| F-06 | Working for tagged unions and opaque |
| F-07 | Working |
| F-08 | Working |
| F-09 | Parse error |
| F-10 | Working for layout intrinsics |
| F-11 | Spec only, full-Zith |
| F-12 | Partial: declared type works, propagation does not |
| F-13 | Working for optional extraction |
| F-14 | In progress, residual facts implemented |
| F-15 | Spec only, full-Zith |
| F-16 | Spec only, full-Zith |
| F-17 | Spec only, full-Zith |
| F-18 | Spec only |
| F-19 | Spec only |
| F-20 | Spec only |
| F-21 | Parse skipped |
| F-22 | Parse skipped |
| F-23 | Parse skipped |
| F-24 | Parse reported and rejected in Zith-- |
| F-25 | Spec only |
| F-26 | Spec only |
| F-27 | Spec only |
| F-28 | Spec only |
| F-29 | Working |
| F-30 | Spec only |
| F-31 | Partial: tagged unions and narrowing work |
| F-32 | Working for validated C, debt remains |
| F-33 | Working |
| F-34 | Working |
| F-35 | Working |
| F-36 | Working |
| F-40 | Working |
| F-41 | Rejected for Zith-- by ADR-0055 |
| F-42 | Working |

The F-10 row is nuanced. `@sizeOf`, `@offsetOf`, `@alignOf`, `@lengthOf`,
`@ptrOf`, and `@canonicalType` are implemented. The generic `@intrinsic`
surface referenced by the older spec name is not, so the row says the layout
intrinsics are working and keeps the Zith-specific surface explicit.

The F-12 and F-13 rows are similarly split. `T!` is a declared type that
lowers through HIR per `impl-status.md`, but `!` propagation is not
implemented in `Zith--`. `raw` optional extraction is working. Zith defines a
separate `unsafe` and raw-block model.

## Zith-Specific Boundaries

Comptime evaluation, reflection/type mutation, capabilities, full NRA,
`fail`/`with`/`catch`/`throw`, words/contexts/use semantics, `::`, assets, and
the stdlib/runtime APIs belong to Zith's separate language track. They are not
missing `Zith--` features. The archived `docs/plans/archive/0.7.0-zith/` tree
is intentionally not active.

Related concepts can have distinct implementations in the two languages.
`T!` is an example: the declared type lowers through HIR in `Zith--`, while
the `!` propagation and `fail` family belong to Zith's separate design.

The C toolchain plans are a separate axis. They are active infrastructure work
for `Zith--` builds, but they are not `Zith--` language features and must not
be confused with the archived Zith plan.

Tag macros are another boundary case. `impl-status.md` says tag macro calls
are parsed and then rejected with `E2010` in the Zith-- pipeline. This is not
a pending port from Zith and should not remain in an active wave checklist
without qualification. The roadmap labels this as parsed/rejected in
`Zith--`; Zith's tag semantics are separate and out of scope for that language.

The dynamic dispatch row in the older spec-only list was also ambiguous. Zith--
supports `dyn Trait` and `dyn Interface` method dispatch through fat pointers
and vtables. The spec-only entry in `impl-status.md` refers to Zith's separate
specification, and `Zith--.md` documents the method-only public surface.
Roadmap text kept the working `dyn` status rather than treating all `dyn`
material as absent.

## Implemented Steps and Debt

`ParseInput` and `InputLine.cast<T>` are shipped for `bool`, `f32`, `f64`,
`i32`, and `u32`. The completed step was moved to
`docs/plans/archive/parse-input-cast.old.md`. `*char` parsing is intentionally
not part of that contract; strings remain available through the `text()`
adapter. The former entry 8 was removed from the active debt inventory, and
the `*char` exclusion remains recorded as non-debt.

Real debt entries must name the implemented-but-incomplete part and a blocking
reference. Intentional design decisions go into the non-debt table. The debt
file does not replace `impl-status.md`; it is the issue tracker for follow-up
work.

The audit in 2026-09-07 tightened the debt classification. `ParseInput` entry 8 was
headlined "deferred" even though the primitives were implemented. The headline
now says the feature is implemented and `*char` is out of scope. Missing
`*char` parsing is intentional because `InputLine.text()` already exposes
strings. That change closes the contradiction between the old headline and the
status row.

After that audit, cache `.zirl` was resolved in commit `e43bdf7` (2026-09-14).
The current debt list below reflects that resolution:

- Bare `opaque` cross-module/cache hydration is closed by the canonical tags
  and `opaque -> opaque` sema no-op; pack/dyn + `opaque` remains separate.
- What Zith-- implements is SRA, frozen by design. The full ownership proof
  (new node-state model, NRA-1..NRA-11) is full-Zith work specified in
  `docs/nra-spec.md`, not Zith-- debt.
- Bare `opaque` has stable tags across modules and cache hydration. The
  canonicalization rule is a single shared function (`types::canonicalTypeId`),
  used by both lowering and cache canonization, with a field-order divergence
  test. A canonical field-order change invalidates old artifacts with `E2010`
  instead of silently re-tagging; the diagnostic does not yet identify the
  specific field that changed.
- C interop imports validated C; support for struct-by-value is deliberately
  limited to record layouts proven for the target. It accepts the single-i64,
  two-i32, two-i64, and exact i32/i64 layouts in either field order, only for
  64-bit x86-64/AArch64 Linux GNU or musl targets. Empty triples use libclang's
  effective target. This boundary is not implementation debt.
- Narrowing `int -> int` checks known integer constants at compile time;
  numeric-literal adaptation lacks range checks, and variable / `float -> int`
  casts lack runtime overflow checks.
- The formatter preserves `for (cond)`, `for {}`, labeled loops and optional
  conditions, while keeping source `while` as `while`.
- `++` and `--` do not exist.
- `..` and `...` each lex as one `Dots` token, distinguished by lexeme.
- `is <type>` narrowing is intentionally limited to tagged unions and
  `opaque` in Zith--; general type tests are not implementation debt.
- Imported trait/interface conformance is deterministic in populated workdirs;
  the remaining section 10 debt is global lookup of trait defaults and
  `dyn Trait` requirements.

The nominal `type` entry is no longer a debt: Zith-- documents the cast-based
contract (`T as Name` / `Name as T`) and keeps `alias` transparent. A dedicated
construction/access syntax remains optional future work.

## Audit Methodology

The practical order used for this task is:

1. Read `TASK.md` and confirm the worktree identity from `agent-id`.
2. Read the active plan files and the current status document.
3. Compare each roadmap feature ID against `impl-status.md` and `Zith--.md`.
4. For stale roadmap rows, update the row instead of removing the feature ID.
5. For shipped steps, move the plan to `docs/plans/archive/`.
6. Update debt entries only when the status document supports the new wording.
7. Run the exact verification commands from `TASK.md`.
8. Review the diff for dead links, contradictory status text, old line counts
   for completed splits, and unintended edits before requesting a merge.

Do not update `impl-status.md` just to make a plan read better. The status
document is verified against compiler behavior and baseline. Only update it
when a status is demonstrably stale, and then verify the new status with a
standalone source file and the relevant focused test.

Do not remove a feature ID from the roadmap merely because it is implemented.
Feature IDs are stable references for commits, tests, and PR descriptions.
Implemented rows should keep the ID and change the status.

Use the grep checks from the task as preliminary evidence only. They find
status labels and pending words, but they do not prove that roadmap prose is
consistent. Read the surrounding wave text after the grep because stale
narrative can survive when the row-level status was already updated.

## Documentation Hygiene

- Update `docs/impl-status.md` only when the compiler behavior is demonstrably
  stale; do not edit it just to make roadmap prose easier.
- When a step ships, move its active plan to `docs/plans/archive/` and
  reference it from `docs/plans/0.7.0/README.md` in the retired-steps section.
- When a roadmap status changes, keep the Wave/01 narrative in sync; stale wave
  prose is a common source of contradictory guidance.
- When debt wording changes from "deferred" to "implemented with out-of-scope
  remainder", update the folder link and the `impl-status.md` line it cites.
- When a completed split changes the file layout, update `monolith-splits.md`
  and `implementation-debt.md` with the new TUs/counts; do not leave old
  ~1789/~1985 numbers as if they were current.
- If a status check fails because an implementation changed, do not classify
  the feature as working from memory. Run the compiler or focused test and
  record the result before editing the status row.
- If a plan mentions a file that no longer exists in `docs/plans/`, point the
  link at the archived `.old.md` or remove the stale reference.
- Prefer small, discrete docs commits. A plan curation change should not hide
  a language-semantics edit.
- Keep memory files focused and within the project's line convention; split a
  growing note instead of letting it exceed the limit.
