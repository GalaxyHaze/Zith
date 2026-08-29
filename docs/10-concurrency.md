## 10. Concurrency & Runtime APIs

> **Implementation status:** concurrency is not a core syntax feature. The compiler does not model
> `async fn`, `yield`, `spawn`, or `await` as language constructs; any future concurrency support is
> expected to arrive through `stdlib` and runtime APIs built from ordinary functions, types, and
> NRA-checked resource rules. See [impl-status.md](impl-status.md).

### 10.1 Core-Language Position

Zith's core language does not define concurrency-specific statements, operators, or function kinds.
There are no dedicated HIR nodes for tasks, threads, `await`, or coroutine suspension. The compiler
understands only:

- ordinary declarations and calls;
- library-defined handle, channel, task, or executor types;
- traits/capabilities used to describe what those types guarantee;
- NRA facts about sharing, lending, capture, escape, and ownership across those calls.

### 10.2 Runtime Surface

The standard library or an alternate runtime may expose APIs such as thread spawners, executors,
message queues, join handles, or resumable tasks. Those APIs are library surface, not syntax:

```zith
let handle = runtime.spawn(workerFn, sharedData);
runtime.join(handle);

let task: Task<Response!> = runtime.schedule(fetchRequest);
let response = runtime.blockOn(task);
```

API names above are illustrative. The compiler does not reserve them.

### 10.3 Thread Fork/Merge

The explicit thread protocol is `fork`/`merge`, backed by the `Branch`
capability. There are no coroutines, `await`, or implicit schedulers:

```zith
let handle = fork Worker(share state);
let result = merge handle;
```

`fork` hands a shared value to a thread and returns a `ForkHandle<T>`; `merge`
blocks, consumes the handle, and restores ownership of the recollected value.
NRA tracks the fork as an ownership transition and rejects unbalanced forks.
See [the branch protocol plan](https://github.com/GalaxyHaze/Zith/blob/main/docs/plans/branch-protocol.md).

### 10.4 What the Compiler Proves

Concurrency-related safety is enforced through the same pre-HIR ownership proof used everywhere
else:

- whether a call duplicates a resource illegally;
- whether a borrowed or `belong` value escapes;
- whether narrowing facts or branch facts justify later lowering decisions;
- whether shared/runtime-managed resources are passed only through the capabilities and wrapper types
  that define the contract.

The compiler does not special-case threads or async control flow. If a runtime API needs stronger
guarantees, it must express them through normal signatures, types, and traits that NRA can reason
about before HIR is finalized.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
