# Named Blocks And `@scoped` Markers

Zith-- has two new statement forms, valid only directly in a `fn`/`state`
body (or as a pipeline stage): a plain named block `setup: { ... }` and a
scope-marker block `render: @scoped(a, b) { ... }`. The design contract lives
in [ADR-0064](../docs/adr/0064-zith-minusminus-named-blocks-and-scope-marker.md),
`docs/Zith--.md`, and `docs/09-control-flow.md` section 9.6. This note records
the non-obvious implementation facts.

## The macro is `scoped`, not `marker`

The spec calls the macro `marker`, but `marker` is a reserved Zith-- keyword
(the removed `flow fn`/`marker` model). The stdlib macro is `scoped`
(`stdlib/std/scope.zithm`). The compiler dispatches on the `#[scope]`
attribute (`AttributeKind::Scope`), never on the macro name, so the name is
not part of the design.

## Architecture: frontend desugaring, not HIR synthesis

`MacroExpander::desugarNamedBlocks` (`src/frontend/macro-expand.cpp`) runs
after template expansion. For a named block whose marker is a visible
`#[scope]` macro, it rewrites the block to, per argument, a hidden
`let __scope$N = arg.enter();` plus `defer __scope$N.leave();` prefixed to the
body. `enter` runs left to right; `leave` runs in reverse through the existing
`defer` machinery, so it fires on fallthrough, `return`, and labeled exits.
The `$` in the generated name can never appear in a user identifier, so the
guard is hygienic. The stdlib macro body is empty.

## Synthetic-cleanup diagnostics

Generated scaffolding statements/expressions carry
`Expression::isSyntheticCleanup` / `Statement::isSyntheticCleanup`. When a
marker argument is rejected, `validateNamedBlock` records those statements in
`skipped_marker_statements_`, `inferBlock` skips them, and the standalone
expression sweep (`src/sema/sema-control.cpp`) ignores them. Without this the
generated `enter`/`leave` calls are re-inferred and bury the dedicated
diagnostic under E3001/E2007/E2001 cascades.

## Position check compares against `decl.body`, not the scope

`is_body_block` must compare the block against
`snapshot.declarations()[currentDeclId_-1].body`, not `expr.scope ==
currentBodyScope_`. A `for` with an init clause wraps the loop in a synthetic
block that reuses the body scope, which would otherwise clobber
`current_body_block_` and make a top-level named block look nested.

## Receiver move and marker-argument reuse

The marker call site is an ordinary method call, so the Zith-- receiver-move
rule applies. A `Scope` implementation with a by-value receiver (`self` or
`var self`) moves the caller binding, and a later read of the marker argument
in the body reports `E4001 UseAfterMove`. An implementation with a borrowing
receiver (`fn enter(self: view Self)` or `self: lend Self`) keeps the argument
usable inside the block and after it. The trait signature in
`stdlib/std/scope.zithm` stays `fn enter(var self)` per the spec; the
implementation chooses `view`/`lend`. `trait(var self)` + `impl(self: view T)`
is accepted because both canonicalize the receiver to `*Owner`
(`src/sema/sema-decl.cpp`, the E2022 receiver-compatibility branch).

## Error codes

`E2032 NamedBlockInvalidPosition`, `E2033 NamedBlockArgNotScope`,
`E2034 NamedBlockDuplicateName` (`src/diagnostics/error-codes.hpp`). Names are
unique per `fn`/`state` body, including collisions with loop labels; names are
never `break`/`continue` targets. Names are preserved in the HIR side table
`HirModule::addNamedScope` (`HirNamedScope`), not serialized into ZIRL and not
part of the ABI.

## Docs caveat

`fmt-check` is unusable locally (project pins clang-format 18/17/16; only
23.1/21.1 are installed, so it flags ~100 untouched files). Verify style by
hand: 100-column limit, 4-space indent, no tabs, no trailing whitespace.
