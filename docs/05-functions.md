## 5. Functions

> **Implementation status:** `fn`, `flow fn`, `raw fn`, and `extern fn` are **working**.
> Overloading ([§5.4](#54-overloading)) is **working**. `const fn` is a **parse error** — `const`
> is a binding keyword; `const fn f()` is parsed as a `const` binding named `fn`. Concurrency is no
> longer a core function kind; any runtime async/task model is expressed through ordinary library
> types and calls. See [impl-status.md](impl-status.md).

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
| `flow fn` | Enables marker/dock control flow ([§9.4](09-control-flow.md#94-flow-functions--markers)). |
| `raw fn` | Always unchecked, bypassing safety in both debug and release. The compiler warns in release builds if `raw` could be removed. |

> Function kinds are orthogonal and cannot be combined on a single declaration.

Macro calls use the `@` prefix — `@println`, `@log`, `@serialize` — while ordinary function calls
use a bare name, such as `console.write`, `process`, or `save`. See [§15](15-macros.md) for the
full rule.

### 5.3 Runtime Tasks, Coroutines, and Concurrency APIs

```zith
// The compiler treats runtime task types like any other library type.
// There is no `async fn`, `yield`, `spawn`, or `await` syntax in the core language.
fn fetch(url: string): Task<Response!> {
    return runtime.schedule(url);
}
```

Concurrency is modeled by `stdlib` or runtime APIs, not by dedicated syntax or a special function
kind. A library may expose `Task<T>`, `Generator<T>`, channels, executors, or thread handles, but
the compiler only sees ordinary declarations, calls, traits/capabilities, and the NRA facts needed
to validate resource usage around them.

### 5.4 Overloading

Several functions in one scope may share a name as long as their parameter lists differ, either in
count or in parameter types. The call site selects the declaration whose parameters accept the given
arguments.

```zith
fn add(a: i32, b: i32): i32 { a + b }
fn add(a: f64, b: f64): f64 { a + b }
fn add(a: i32, b: i32, c: i32): i32 { a + b + c }

fn main(): i32 {
    let i: i32 = add(1, 2);        // add(i32,i32)
    let f: f64 = add(1.0, 2.0);    // add(f64,f64)
    return add(i, 3, 4);           // add(i32,i32,i32)
}
```

Methods overload the same way; the implicit `self` parameter participates in the signature.

```zith
implement Point {
    fn shifted(self): Point { ... }
    fn shifted(self, by: i32): Point { ... }
}
```

Rules:

- Two declarations whose parameter types are identical are a duplicate declaration (`E2002`), even
  when their return types differ. The return type is never part of overload selection.
- Memory qualifiers do not discriminate overloads: `fn f(p: lend P)` and `fn f(p: view P)` are the
  same signature, and therefore `E2002`.
- A function name may not collide with a non-function binding of the same name (`E2002`).
- `extern fn` cannot be overloaded: it carries a fixed C linkage name.
- A call with no candidate that accepts the arguments is `E2007`; a call accepted by more than one
  candidate is `E2008`. There is no ranking of conversion quality, so any tie is an error rather
  than a silent choice.
- Name resolution does not merge candidates across scopes. The nearest scope declaring the name
  wins and shadows outer declarations entirely.

Overloading is implemented by qualifying linkage names as `<module>.<Owner>.<name>(<params>)`, for
example `std.io.console.println(*char)`. `extern fn` declarations and `main` keep their plain source
name so that C interop and the linker's entry point are unaffected.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
