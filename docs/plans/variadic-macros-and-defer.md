# Variadic Macro Repetition And Caller-Scope Defer

Status: accepted design, implementation pending

Zith-- macros currently require exact arity, so a macro cannot generate one
statement for each item in a heterogeneous argument list. A fixed-arity
`raw macro` can name each resource explicitly, but it cannot scale to an
arbitrary number of resources. The motivating example is
`@deferDestroy(vec, arena)`, which should register cleanup for each identifier and defer
those statements until the lexical block containing the call exits.

Size: language subsystem, macro declaration parsing, expansion, and cleanup
scope interaction in Zith--. The design is accepted, but no implementation
order has been set.

## Current Facts

- Zith-- supports `macro` and `raw macro` with fixed arity. Macros with the
  same name cannot be overloaded by arity.
- Function variadic slices such as `[...]T` collect homogeneous values. They
  do not provide syntax-level expansion for heterogeneous arguments.
- A `defer` registers cleanup with its nearest lexical block. Statements in a
  deferred block run in written order, while separate defers run in reverse
  registration order. See [scope cleanup](../09-control-flow.md).
- A `raw macro` that expands to `defer` currently associates cleanup with the
  macro expansion boundary. An observed example runs the cleanup before the
  next statement after the macro call. The target behavior is different:
  cleanup belongs to the lexical block containing the call.

## Accepted Design

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
This is accepted design, not implemented syntax. See
[ADR-0055](../adr/0055-zith-minusminus-explicit-ownership-and-pipelines.md)
for the ownership and pipeline contract.

## Non-goals

- Does not change function variadic slices or add a general-purpose spread
  operator.
- `@deferDestroy` does not accept expressions. Its arguments are identifiers.
- Does not change the cleanup order of separate `defer` statements.
- Does not add automatic cleanup or a language-level `drop` feature.
- Does not add thread-specific macro behavior. This is a general Zith--
  language capability, independent of [`soon/thread`](stdlib/soon-thread.md).

## Implementation Questions

- Whether postfix repetition applies only to expression statements initially
  or to every statement form.
- How expansion preserves the caller's lexical cleanup scope through nested
  expansions, control-flow exits, and imported macros.
- Which diagnostics cover a variadic parameter used outside a repeated
  statement, or used with an unsupported argument kind.
