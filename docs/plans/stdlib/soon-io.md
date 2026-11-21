# `soon/io` Executor And Non-Blocking I/O

Status: drawing

The standard library has no execution model for work that must run off the
calling thread. The `soon/fs` stub declares `open`, `read`, and `write` but
states no blocking contract, so a caller cannot tell whether a read waits for
the operating system or returns immediately. The proposed surface keeps the
plain call blocking and adds an executor overload: `read(path)` stays an
ordinary blocking function, while `read(executor, path)` hands the operation to
an executor and returns a handle for the eventual result. The executor runs
submitted state machines step by step, so the work it accepts is expressed with
the same explicit `state` transitions the language already has. This drawing
records the executor, task, and cancellation contract.

Size: module, the standard-library I/O execution surface.

## Current Facts

- Zith-- has no `async`, `await`, coroutine, or implicit scheduler syntax. A
  library or runtime may expose executors, tasks, and handles as ordinary
  types, but the compiler only sees ordinary calls, traits, and NRA facts.
- A `state` function declares one state. `dock Start(args)` runs a machine to
  its return value on the current thread, and `jump Next(args)` transfers
  directly to the next state. Neither suspends nor schedules work by itself.
- The `soon/thread` `ThreadBackend` trait launches a `Job` and returns a
  `ThreadHandle<T>` whose `merge` blocks the current thread and consumes the
  handle once. It defines no resumable task and no executor queue.
- `stdlib/soon/fs/fs.zithm` is an experimental stub. It declares the file
  operations but promises no runtime contract, so the blocking behavior of
  `read` and `write` is still open.
- The VM v2 backend accepts a validated extern set, and WASM stubs several host
  operations. A worker pool needs a runtime that can start threads on each
  backend, which is not part of the current `soon/` surface.
- The glossary already uses `Thread handle` for the single-consumer `Thread<T>`
  value and avoids the word `task` for it. The executor task in this drawing is
  a separate concept and must not be confused with a thread handle.

## Proposed Surface

Exact declaration syntax and type names are illustrative.

```zith
import soon/io as io

fn main() {
    // Blocking call. Runs on the calling thread.
    let bytes = io.read("config.txt");

    // Non-blocking call. The executor runs the read on a pool worker and
    // returns a handle immediately.
    let rt = io.Executor.new();
    let task = io.read(rt, "config.txt");

    // Inspect without blocking, then wait explicitly for the result.
    let status = task.status();
    let result: Result<[]char, IoError> = task.wait();
}
```

The overload set is `read(path)` and `read(executor, path)`. The plain form
stays a blocking ordinary function. The executor form submits work and returns
an `IoTask<T>` handle. `read(executor, path)` is a convenience wrapper over the
generic submission `executor.submit(state, context)`.

## The Execution Model

An executor owns a queue and a worker pool. `Executor.new()` creates or obtains
an executor bound to that pool. The executor does not represent one thread. A
worker is one thread that runs a step, and different steps of the same task may
run on different workers.

A submitted task is a state machine composed of several steps. The executor
owns the task context and lends it exclusively to a step for the duration of
that step. A step receives the same explicit context type as every other step
of the task. When the step ends it returns either a continuation, naming the
next step, or a terminal value. The executor re-enqueues a continuation on the
queue rather than running it inline, so every step yields control and no task
can starve the others. When the executor reads a terminal value, the task is
complete and the value becomes the handle result.

A terminal value carries `Result<T, IoError>`. A failed read is a result of the
operation, not a failure of the executor. The task still completes normally and
the error is read from the handle.

The `IoTask<T>` handle exposes three operations:

- `status()` returns the current status without blocking.
- `tryResult()` returns the result without blocking, when one is ready.
- `wait()` blocks the calling thread until the task completes and returns the
  result.

The result is consumed once. `status()` and `tryResult()` do not consume it.

## Cancellation And Lifecycle

Cancellation and submission control are separate operations:

- `cancelAll()` cancels the tasks that are still queued, lets each step that is
  already running finish, and marks the task `canceled` without running its
  continuation. It returns immediately. It does not terminate worker threads.
- `cancel(task)` cancels one task under the same rule.
- `pauseSubmissions()` stops accepting new submissions while the tasks already
  accepted continue.
- `resumeSubmissions()` starts accepting new submissions again.

`cancelAll()` does not close the executor. After the running steps finish and
the tasks reach `canceled`, the executor accepts new submissions normally.
`pauseSubmissions()` is independent of cancellation and is reversible.

## Non-goals

- Does not add `async`, `await`, coroutines, or a scheduler as language syntax.
- Does not change `state`, `jump`, or `dock`. `dock` still runs a machine to its
  return value on the current thread.
- Does not make the plain `read(path)` non-blocking. Synchronous operations
  stay ordinary functions.
- Does not expose a worker thread as the executor. Affinity to a specific
  worker, if ever needed, is a separate abstraction.
- Does not define forced termination of a worker that is blocked in a host
  operation. `cancelAll()` waits for the in-progress step to return.
- Does not define the final file-system API, error type names, or buffer
  ownership rules of `soon/fs`.
- Does not compose handles into a combined task in this first cut. A workflow
  that spans steps is submitted as one state machine.

## Open decisions

- The exact spelling and generic signature of `executor.submit`, and how the
  step context type is declared and constrained.
- The concrete type and method names of the task handle, its status enum, and
  the terminal continuation union.
- Whether the continuation is a state value, a small enum, or a distinct
  executor step type, and how a step names its successor.
- How the executor runtime starts and sizes its worker pool on the native, VM
  v2, and WASM backends, and what happens when a backend cannot start threads.
- Whether `cancel(task)` may also request cancellation of a step already in
  progress, and how a long host operation observes that request.
- How `read(executor, path)` reports a submission rejected by
  `pauseSubmissions()`.
- Where the executor and task types live: a new `soon/io` module, the existing
  `soon/fs`, or a shared runtime module.
