# Variadic Macro Repetition And Caller-Scope Defer

Status: implemented

The design addressed the former exact-arity limit on Zith-- macros, which
prevented one macro from generating a statement for each item in a
heterogeneous argument list. A fixed-arity `raw macro` could name each resource
explicitly, but could not scale to an arbitrary number. The motivating use is
`@deferDestroy(vec, arena)`, which registers cleanup for each identifier until
the lexical block containing the invocation exits.

The implementation covers macro declaration parsing, expansion, and cleanup
scope interaction in Zith--. The normative contract is in
[the feature spec](../specs/variadic-macros-and-defer.md).

## Implemented Design

Normal and raw macros may declare one final variadic parameter. It declares an
existing macro argument kind such as `val`, `identifier`, or `ast`. Its tail
accepts zero or more arguments, and call syntax remains an ordinary
comma-separated argument list.

The postfix `...` on a template statement repeats it once for each captured
argument, in input order. `@deferDestroy` specifically accepts identifiers:

```zith
raw macro deferDestroy(names: identifier...) {
    defer {
        names.destroy()...;
    }
}

@deferDestroy(vec, arena);
```

The expansion registers one deferred body in the caller's lexical block. That
body calls `vec.destroy()` followed by `arena.destroy()`. An empty variadic
tail emits no cleanup. Separate generated or user-written `defer` statements
retain the existing reverse registration order.

After registration, each owner remains usable until the block exits, but
cannot be moved, rebound, or explicitly destroyed before the deferred call.
Existing ownership checks continue to apply. See
[ADR-0055](../adr/0055-zith-minusminus-explicit-ownership-and-pipelines.md)
for the ownership and pipeline contract.

## Non-goals

- Does not change function spread semantics. The implemented `...slice`
  forwarding marker is specified in
  [ADR-0060](../adr/0060-explicit-variadic-slice-forwarding.md). General pack
  spread is a separate accepted design in
  [ADR-0061](../adr/0061-zith-minusminus-pack-spread.md), not part of this plan.
- `@deferDestroy` does not accept expressions. Its arguments are identifiers.
- Does not change the cleanup order of separate `defer` statements.
- Does not add automatic cleanup or a language-level `drop` feature.
- Does not add thread-specific macro behavior. This is a general Zith--
  language capability, independent of [`soon/thread`](stdlib/soon-thread.md).

## Verification

Focused frontend, macro-expansion, and codegen tests cover variadic
declarations, argument validation, statement repetition, caller-block cleanup,
empty tails, and defer ordering.
