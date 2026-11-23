## 5. Functions

> **Status:** see [impl-status.md](impl-status.md) for the current status of each function kind.
> Concurrency is not a function kind: any runtime async/task model is expressed through ordinary
> library types and calls.

### 5.1 Return Types & Implicit Returns

Non-void functions may use an implicit return only when every possible path produces a value or
otherwise terminates. A final expression with the declared type and complete `if`/`else` or
`when` bodies are accepted. Falling off the end of an `if` without `else`, a `when` without a
default, or an empty body is a diagnostic, not an implicit `null`.

```zith
fn add(a: i32, b: i32): i32 { a + b }   // explicit type, implicit return
fn add(a: i32, b: i32)      { a + b }   // inferred type

// Bounds-checked indexing: returns the element if in range, otherwise
// propagates null via the implicit optional from '?'.
fn first<T>(slice: []T): ?T {
    slice[0]?
}

fn pick(flag: bool): i32 {
    if (flag) {
        1
    } else {
        2
    }
}
```

> The compiler cannot infer a `union` or `dyn` return type without an explicit type hint.

### 5.2 Function Kinds

This table describes the Zith-- function surface. Full Zith classifies
callables with the separate `safe`, `trust`, and `hazard` levels in
[ADR-0043](adr/0043-full-zith-safety-levels-safe-trust-hazard.md).

| Kind | Description |
|---|---|
| `fn` | Standard runtime function. |
| `const fn` | Compile-time function; parsing is in progress, evaluation is not implemented yet. |
| `state` | A state in a machine with one return type and possibly diverging parameters; `dock State(args)` starts the machine and `jump Next(args)` terminates a state with a direct LLVM `tailcc`/`musttail` transition ([§9.4](09-control-flow.md#94-state-functions-and-state-machines)). |
| `raw fn` | Zith-- only: calls require an explicit `raw` call-site marker. The function body remains checked. |
| `extern fn` | Fixed C ABI linkage; never name-qualified and never overloaded. |

An ordinary `fn` can also be declared without a body and linked to a C symbol by
placing the full Zith signature on the left and the C identifier on the right:

```zith
fn destroy(self: lend Window): void = extern SDL_DestroyWindow;
```

Method binders that need a receiver should be declared inside an `implement`
block, which is what gives `window.destroy()` its normal method semantics:

```zith
implement Window {
    pub fn destroy(self: lend Window): void = extern SDL_DestroyWindow;
}
```

The Zith name keeps normal module, overload, and receiver semantics. The right-hand
identifier is used as the C ABI linker symbol. The compiler emits an external
declaration with that name and never generates a body. This form is not allowed on
`const fn`, `state`, trait requirements, or interface requirements.

> In Zith--, `raw fn` and `extern fn` are separate concerns: `raw fn` requires
> an explicit marker at each call site, while `extern fn` selects the C ABI.
> Combining them as `extern raw fn` is deferred.

Macro calls use the `@` prefix, such as `@println`, `@log`, and `@serialize`, while ordinary function calls
use a bare name, such as `console.write`, `process`, or `save`. See [§15](15-macros.md) for the
full rule.

### 5.3 Runtime Tasks, Coroutines, and Concurrency APIs

```zith
// The compiler treats runtime task types like any other library type.
// Thread protocols use fork/merge; Task-style scheduling is stdlib surface.
fn fetch(url: string): Task<Response!> {
    return runtime.schedule(url);
}
```

Concurrency is modeled by `fork`/`merge` plus `stdlib`/runtime APIs, not by a
function kind such as `async fn`. A library may expose `Task<T>`, `Generator<T>`,
channels, executors, or thread handles, but the compiler only sees ordinary
declarations, calls, traits/capabilities, and the NRA facts needed to validate
resource usage around them.

`async`/`await` is out of scope for the current design, and a normal
`async`/`await` model is expected to remain banned. A suspension point would
hide ownership and lifetime effects from the NRA, so asynchronous execution is
left to runtime APIs or `context` surfaces whose contract the compiler can
validate. `state` is the intended alternative, because its suspension and
resumption points are explicit and can reproduce the behaviour of an `async`
when a program needs it.

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

Methods overload the same way. The implicit `self` parameter participates in the signature.

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
- Only `extern fn` may declare C-style variadic parameters with a standalone `...` marker.
  Generic function type packs use the separate `Ts...` syntax described below.
- `fn f = extern CSymbol` may be overloaded: each overload may select a distinct C symbol.
- A call with no candidate that accepts the arguments is `E2007`. A call accepted by more than one
  candidate is `E2008`. There is no ranking of conversion quality, so any tie is an error rather
  than a silent choice.
- Name resolution does not merge candidates across scopes. The nearest scope declaring the name
  wins and shadows outer declarations entirely.

Overloading is implemented by qualifying linkage names as `<module>.<Owner>.<name>(<params>)`, for
example `std.io.console.println(*char)`. `extern fn` declarations and `main` keep their plain source
name so that C interop and the linker's entry point are unaffected.

### 5.5 Function Type Packs

**Accepted design, not implemented.** A Zith function may declare one final
generic type pack. The function binds its final call arguments to a tuple-valued
parameter:

```zith
fn invoke<Ts...>(args: Ts): void {
    target(...args);
}
```

`args` is the inferred tuple as a whole. `...args` expands its elements as
separate positional arguments. The tail may be empty. Passing an explicit tuple
to `invoke` contributes one argument, so `args` contains that tuple as one
element rather than flattening it.

Fixed generic parameters must precede `Ts...`. Calls may infer the pack or
specify it explicitly. `...Ts` expands the collected types in a generic
argument list, and a constraint on `Ts` applies to each type in the pack. Each
value follows the usual ownership rules.

A function may declare either one heterogeneous type pack or one homogeneous
variadic slice `[...]T`, but not both. The type pack is limited to functions.
Structs, aliases, enums, unions, and traits cannot declare one because tuples
and comptime provide type-level aggregation without a second open-ended arity
mechanism. The accepted trade-off is recorded in
[ADR-0062](adr/0062-zith-function-type-packs.md).

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
