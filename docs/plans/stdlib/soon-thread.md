# `soon/thread` Thread Library For Zith--

Status: drawing

Zith-- has no thread surface. The `c/` tree has no thread binding, `stdlib/`
has no thread module, and the native, VM v2, and WASM backends disagree about
what a thread even is. The full-Zith thread plan (`fork`, `merge`, `revoke`,
`spawn`) is spec-only and depends on the ownership type system that Zith-- does
not have. This drawing records a thread library for Zith-- that is ordinary
runtime code: the compiler sees only ordinary functions, traits, and results,
and adds no ownership rule for threads. The surface is `spawn(entry)` to build
a `Job` blueprint, `job.unbounded(args)` to launch it, and a `ThreadBackend`
trait with a default `Thread` implementation. This drawing fixes the backend
protocol, the handle lifecycle, the sharing wrappers, and the error channel.

Size: module, the standard-library thread surface.

## Current Facts

- Zith-- has no `fork`, `merge`, `revoke`, or `spawn`. All four are `Spec only`
  for full Zith in `docs/impl-status.md`, and `docs/Zith--.md` does not mention
  threads at all.
- No thread binding exists in `stdlib/c/` or `stdlib/std/`, and there is no
  `stdlib/soon/thread`. The native backend links libc but binds no `pthread`
  call. The VM v2 backend accepts a validated extern set, so a new thread
  operation needs a VM handler. WASM has no `wasi_thread_spawn`, so the browser
  backend cannot start a real thread today.
- Zith-- has no ownership type system for threads. It rejects `share`,
  `unique`, and `belong` with `E2010` and keeps only `lend` and `view`. A
  library that moves data into a thread therefore relies on runtime discipline,
  not a compiler rule.
- Zith-- has no closures with captures. Only anonymous `fn(...)` literals
  without capture exist, so an entry cannot close over caller state. Launch
  arguments must be passed explicitly.
- An entry may be a plain function or a state machine. `state`, `dock`, and
  `jump` work. A function value `fn(A): R` can be stored in a struct field and
  called through it, so a `Job` can hold its entry as a field. Only concrete
  non-generic function types are exercised today.
- `defer` runs on every lexical exit, including `return`, `break`, `continue`,
  and `jump`. Zith-- has no destructors, so cleanup is an explicit `destroy`
  call or a `defer`.
- `Result<T, E>` is the canonical failable return. The `T!` failable form is
  outside Zith--.
- A macro is invoked as `@name(args)` and takes a block only as an ordinary
  argument, so the synchronization sugar is `@sync(val, { ... })`. The
  trailing-block form `@sync(val) { ... }` is not accepted and reports a macro
  arity error.
- The `soon/io` executor drawing builds its worker pool on top of this module,
  so this thread surface is the lower layer.

## Proposed Surface

Exact declaration syntax and error type names are illustrative.

```zith
import soon/thread as th

fn worker(ctx: th.Context): i32 {
    ctx.done(7);
    7
}

fn main() {
    let backend = th.Thread.new();
    let job = backend.spawn(worker);      // blueprint, does not launch
    let handle: th.ThreadHandle<i32> = job.unbounded(ctx);
    let result: Result<i32, th.ThreadError> = handle.merge();
}
```

`Thread` is the default backend and implements the `ThreadBackend` trait.
Other backends are explicit values. `spawn(entry)` builds a `Job` blueprint and
does not start a thread. `job.unbounded(args)` launches the work and returns a
`ThreadHandle<R>`, where `R` is the entry's return type. The launch returns
`Result<ThreadHandle<R>, ThreadError>` so a backend that cannot start a thread
reports `ThreadError.Unsupported` instead of trapping.

The entry is a concrete, non-generic function that receives one explicit
context argument. The context type is shared by every thread of one backend.
The context is the runtime's, not a captured environment, because Zith-- has no
captures.

## The Backend Protocol

`ThreadBackend` is a trait. It names the ability to build a blueprint and to
launch it, and it is the seam that lets the native, VM v2, and WASM backends
implement the same surface. `Thread` is the default implementation and is
selected when the caller does not name a backend.

The backend is an explicit value, not ambient global state. A program that
needs a specific backend names it and passes it in. When a backend cannot
provide real threads, `job.unbounded` returns `ThreadError.Unsupported` and the
caller decides whether to fall back to a blocking call.

## Blueprint And Handles

`Job` is a reusable description of work. It holds the entry and nothing else,
because Zith-- has no captures. Launch arguments are supplied at the launch
site. A blueprint may be launched more than once.

Two handle kinds are distinct:

- `ThreadHandle<T>` is the mergeable handle. `merge()` blocks the calling
  thread, consumes the handle, and returns the entry's result. It is the same
  single-consumer value the full-Zith `merge` keyword consumes.
- A monitoring handle tracks a launched execution without consuming a result.
  It is not consumed by `merge`. Its exact type and method set remain open.

The first cut ships only `job.unbounded(args)`. A bounded launch and its
completion and resource-release rules are a documented library rule, not a
compiler rule, and are deferred.

An unconsumed `ThreadHandle<T>` at scope exit is implicitly detached. Detach
means the runtime stops tracking the thread and never destroys the handle.
Because Zith-- has no ownership enforcement, the runtime does not fail an
unconsumed handle. An explicit `detach()` method is deferred.

## Sharing Wrappers

Zith-- has no compiler-enforced sharing, so the library supplies two runtime
wrappers:

- `Share<T>` lets several threads read or update one value through explicit
  methods. Access is by method, not by field.
- `Revokable<T>` is a reference whose access the owner may revoke. A failed
  access returns `Result<T, ThreadError>` with the `Revoked` variant. The owner
  revokes without destroying the value or terminating the child.

These are library runtime wrappers, not the full-Zith revocable type qualifier.
ADR-0034 fixes the full-Zith spelling as a `'` sigil before the type, with no
source-level `Revokable<T>` wrapper. Zith-- has no such qualifier, so the
library wrapper is the only spelling available in the subset. The two are
different mechanisms that share a word, and the drawing keeps them apart.

### Proposed Synchronized Access

This is a proposed runtime contract, not an implemented API or a concrete
Zith-- signature. If a `Share<T>` is registered with a `Disposer`, the
`Disposer` remains responsible for destroying `T`. Copying a `Share<T>` copies
an access handle, not ownership of `T` or an extension of its lifetime. The
control block remains valid while handles exist, including after `T` has been
destroyed.

`sync(share, action)` waits while another action holds the share, then calls
`action` with a `lend T`. `trySync(share, action)` does not wait and reports
`Busy` when access is contended. Both report `Closed` if teardown begins
before the action can start. The lock stays internal. The action receives
neither a mutex nor a guard.

Teardown closes admission to new actions. Calls waiting for access when
teardown begins fail with `Closed`, while actions already running are allowed
to finish. The `Disposer` destroys `T` only after those active actions have
finished. A surviving handle receives `Closed` instead of accessing the
destroyed value.

Access failures are distinct from errors returned by the action. Conceptually,
the result is `Result<R, SyncError<E>>`, where `SyncError<E>` distinguishes
`Closed` from `Action(E)`, and `trySync` also distinguishes `Busy`. The exact
type and signature remain subject to the Zith-- type system.

The lock is released on every action exit, including an action error. An action
must not re-enter `sync` or `trySync` on the same `Share`, and must not return
or retain its `lend T`. Until the compiler can enforce non-escape, this is a
manual precondition. A future `noescape`-style annotation may express it.

## Synchronization Sugar

A lock and unlock pair is expressible with a macro and `defer`:

```zith
macro sync(val: identifier, body: block) {
    val.lock();
    defer val.unlock();
    body;
}

@sync(mutex, { counter = counter + 1; });
```

The block must be an ordinary argument, so the call is `@sync(val, { ... })`.
The trailing-block form `@sync(val) { ... }` is not part of the macro call
grammar and fails with an arity error. Supporting that form would need a parser
change and is out of scope here.

A `raw` macro does not rescue the trailing-block spelling. A raw macro is
invoked the same way, `@sync(val)`, and its body is spliced as statements. The
splice runs before the following block, and the `defer` it introduces is
scoped to the macro expansion, so `val.unlock()` runs before the following
block starts. The block then runs unlocked, which is a silent correctness bug
rather than a syntax error. The block-as-argument form keeps `lock` and
`unlock` around the block because the block is the macro's own statement. Use
`@sync(val, { ... })`.

The block-as-argument spelling is accepted. The `@sync(val, { ... })` form is
the shipped spelling for the synchronization sugar, and no parser change is
wanted for it. The trailing-block form stays out of scope.

Variadic macro repetition and caller-scope cleanup are tracked in the separate
[language drawing](../variadic-macros-and-defer.md), not as part of this thread
module.

A macro can be exported with `pub` and called through a module alias, so
`@share.sync(val, { ... })` works when the macro is public in the imported
module. The sugar can therefore live beside `Share` in a `soon/thread/share`
module and still read as a call on the share surface.

## Errors

`ThreadError` is an enum with named variants. The first variants are
`Unsupported`, for a backend that cannot start a thread, and `Revoked`, for a
failed `Revokable` access. Thread creation errors are separate from the entry's
own result, so a failing entry still returns through `merge` as its own
`Result`, not as a `ThreadError`.

## Non-goals

- Does not add `fork`, `merge`, `revoke`, or `spawn` as compiler keywords. The
  library `spawn` builds a blueprint and the library `merge` is a method.
- Does not add ownership or type-system rules for threads. Zith-- stays on the
  normal runtime path.
- Does not add closures or captures. Launch arguments are explicit.
- Does not define VM v2 or WASM thread execution. Those backends report
  `ThreadError.Unsupported` until a runtime exists.
- Does not define the executor, task, or non-blocking I/O surface. That is the
  `soon/io` drawing, which builds on this module.
- Does not define a bounded launch, its completion rule, or its resource
  release in the first cut.
- Does not promise cross-backend scheduling or affinity guarantees.

## Open decisions

- The exact spelling of `spawn`, `Job`, `ThreadHandle`, and the context type,
  and whether `spawn` is a method on the backend or a free function.
- Whether the entry is a bare function value, a small entry struct, or a
  state-machine value, and how the context type is declared and constrained.
- The method set of the monitoring handle and how it observes completion
  without consuming a result.
- Whether an explicit `detach()` method exists, or detach stays implicit only.
- Whether the first cut ships a bounded launch beside `unbounded`, and the
  bounded completion and resource-release rules.
- The remaining `Share<T>` method surface and concrete `sync`/`trySync`
  signatures and error type, plus the `Revokable<T>` contract and how the owner
  revokes and waits for acknowledgement.
- The full `ThreadError` variant list and which variants each backend returns.
- How `soon/thread` starts a real thread on native, VM v2, and WASM, and what
  `c/thread.<os>.zithm` exposes as raw externs.
- Whether the synchronization sugar lives in `soon/thread` or beside `Share` in
  a `soon/thread/share` module. Its spelling is settled as
  `@sync(val, { ... })`, and a public macro called through a module alias is
  possible, so `@share.sync(val, { ... })` is expressible.
