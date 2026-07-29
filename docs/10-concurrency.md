## 10. Concurrency & Threads

> **Implementation status:** `async fn` is accepted by the parser but the body is skipped
> (**parse skipped**). `yield`, `spawn`, and `await` are **spec-only** with no compiler support.
> See [impl-status.md](impl-status.md).

### 10.1 Spawning

```zith
// Default thread type
let handle = spawn worker_fn(data);

// Explicit thread type
let handle = spawn worker_fn(data) with GreenThread;

// Fire-and-forget
let _ = spawn background_task();

// #wont_remain -- promise the thread dies before the scope ends
#wont_remain let _ = spawn quickTask(sharedData);
```

### 10.2 Compile-Time Safety Enforcement

If a thread accesses shared data, the compiler requires **one of**:

- `await handle` before the shared data goes out of scope, or
- the `#wont_remain` attribute on the spawn.

Missing both is a **compile error**. Violating the `#wont_remain` promise is not caught by the compiler — the shared data may become a dangling reference. An alternative to both is using `global: share T` or `Rc`.

| Keyword / Method | Semantics |
|---|---|
| `await handle` | Waits for the thread to finish. |
| `await globalVar` | Blocks until the global variable has been initialized. |
| `handle.send(msg)` | Sends a message to the running thread. |
| `#wont_remain` | Attribute on `spawn`. Promises the thread dies before the enclosing scope ends. |

```zith
let handle = spawn worker(sharedData);
await handle;

global cfg: Config;
await cfg;
@println(cfg.host);
```

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
