## 5. Functions

> **Implementation status:** `fn`, `flow fn`, `raw fn`, `extern fn`, and `async fn` (body skipped)
> are **working** or **parse skipped**. `const fn` is a **parse error** — `const` is a binding
> keyword; `const fn f()` is parsed as a `const` binding named `fn`. `async fn` body lowering and
> `yield` are **spec-only**. See [impl-status.md](impl-status.md).

### 5.1 Return Types & Implicit Returns

```zith
fn add(a: i32, b: i32): i32 { a + b }   // explicit type, implicit return
fn add(a: i32, b: i32)      { a + b }   // inferred type

// Bounds-checked indexing: returns the element if in range, otherwise
// propagates null via the implicit optional from '?'.
fn first<T>(slice: []T): ?T {
    slice[0]?
}
```

> The compiler cannot infer a `union` or `dyn` return type without an explicit type hint.

### 5.2 Function Kinds

| Kind | Description |
|---|---|
| `fn` | Standard runtime function. |
| `const fn` | Resolved entirely at compile time. |
| `async fn` | Returns `Coroutine<T>` — a lazy generator with a default base implementation. Must be consumed by a type implementing `Generator`. |
| `flow fn` | Enables marker/dock control flow ([§9.4](09-control-flow.md#94-flow-functions--markers)). |
| `raw fn` | Always unchecked, bypassing safety in both debug and release. The compiler warns in release builds if `raw` could be removed. |

> Function kinds are orthogonal and cannot be combined on a single declaration.

Macro calls use the `@` prefix — `@println`, `@log`, `@serialize` — while ordinary function calls use a bare name, such as `console.write`, `process`, or `save`. See [§15](15-macros.md) for the full rule.

### 5.3 `async fn` & `yield`

```zith
// An async fn returns Coroutine<T>, not T directly.
// Coroutine<T> has a default base implementation.
// Only types implementing Generator can receive it.
async fn fetch(url: string): Response! {
    yield;
    get_response()!
}
```

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
