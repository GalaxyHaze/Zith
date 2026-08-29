# Branch Protocol Design

## Status

Draft design for the core `Branch` capability, `fork`, and `merge`. This document
specifies the intended language contract; implementation is not started.

## Goal

Zith keeps `state` machines as the deterministic control-flow mechanism and
explicit threads as the only concurrent execution boundary. The `Branch`
protocol defines how a shared value may leave the current function, run inside a
thread, and be recollected with ownership restored.

The protocol deliberately avoids `async`, `await`, futures, and an implicit
runtime scheduler. `merge` is blocking, one-shot, and consumes the `ForkHandle`.

## Terminology

| Term | Meaning |
|---|---|
| `Branch` | Capability that a type implements to support fork/merge. |
| `fork` | Keyword equivalent to spawn: hands a shared value to a new thread and returns `ForkHandle<T>`. |
| `merge` | Keyword equivalent to join plus value collection: waits for the thread, consumes the handle, restores ownership. |
| `ForkHandle<T>` | Owned, single-consumer handle produced by `fork`. |
| `state*` | Future design: function-pointer-like entry that refers to a `state` machine. |
| `forkCount` | Ownership counter tracked by NRA for a `share` node. |

## Capability Definition

```zith
capability Branch {
    fn fork(self: share Self): ForkHandle<Self>;
    fn merge(self, handle: ForkHandle<Self>): Self;
}
```

The compiler knows these two operations by name for ownership purposes. User
code implements the capability on the concrete shared type; the stdlib runtime
connects a `fork` expression to a thread entry.

## Syntax

```zith
let handle = fork Worker(share state);
let result = merge handle;
```

Initial design:

- `fork ThreadEntry(share value)` requires the argument type to implement
  `Branch` and `Share`.
- `merge handle` requires `handle: ForkHandle<T>` and returns `T`.
- `merge` may be written `merge handle` with the handle consumed as a value.
- No public method form is required for ownership; keywords give the protocol an
  explicit surface and avoid pretending the operation is an ordinary library call.

## `state*` Entry

The original thread model is based on `state` machines, not closures. A future
`state*` spelling is a function-pointer-like reference to a state machine:

```zith
state* Worker { ... }

let handle = fork Worker(share state);
let result = merge handle;
```

`state*` is not part of the first Branch implementation. `fork` may initially
accept ordinary `state` functions or an explicit thread-friendly signature.

## NRA Contract

Every `share` node that is passed to `fork` carries a `forkCount`:

```text
0: the node is still owned/controlled by the source.
1: the node is handed to one branch and is pending merge.
>1: invalid for this model; a second fork while one is live is rejected.
```

Transition rules:

1. `fork(share value)` increments `forkCount` and creates a `ForkHandle<T>`.
2. While `forkCount != 0`, the source may not be moved, reassigned, or forked
   again.
3. `merge handle` must consume the handle exactly once.
4. `merge` decrements `forkCount` and returns the recollected value/state.
5. Ending a scope with `forkCount != 0` or with a live `ForkHandle` is an
   ownership error.

The compiler emits diagnostics such as:

- `fork still live` for an unmerged fork at scope exit;
- `fork source busy` for a second fork while one is pending;
- `handle abandoned` for a `ForkHandle` that is dropped without `merge`.

`ForkHandle<T>` is an owned, single-consumer value. It is not `share`, `view`,
or `lend`; only `merge` consumes it.

## Runtime Surface

The runtime/stdlib exposes thread entry, wait, and result transport, but these
are ordinary implementations that only the `Branch` capability wires into NRA:

```zith
struct WorkerHandle<T> {
    // runtime state: thread, result, join primitive
}

implement WorkerHandle<T> as Branch {
    fn fork(self: share Self): ForkHandle<T> { ... }
    fn merge(self, handle: ForkHandle<T>): Self { ... }
}
```

Implementation notes:

- `fork` and `merge` may be supplied by the runtime for types that prove
  `Share` and implement `Branch`.
- No coroutine suspension point exists. `merge` blocks the current thread until
  the branch completes.
- No `await` syntax, no `Task<T>` scheduler, and no resumable future is added by
  this protocol.

## Comptime Role

`comptime` is not the engine of fork/merge. It may generate wrappers, merge
strategies, or friendly APIs over the protocol, but it cannot validate runtime
ownership because resources and scopes are outside the comptime value domain.

## Open Questions

- Whether `fork` may accept an ordinary `fn` initially or must wait for `state*`.
- Whether a detached thread is allowed and, if so, how the abandoned result is
  reclaimed.
- Whether the capability should be named `Branch` in the reserved capabilities
  registry from the start.
- Whether `merge` may appear as a method for internal/runtime use in addition to
  the keyword.

## Related Roadmap Items

- F-20: NRA shared-resource facts for runtime concurrency APIs.
- F-18/F-19: runtime task/thread APIs.
- F-14: full NRA/ownership proof.
