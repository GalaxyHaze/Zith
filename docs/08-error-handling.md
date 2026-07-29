## 8. Error Handling

> **Implementation status:** `?T` and `T!` types are **working**. `?` postfix propagation is
> **working** with full operand and return-type validation. `null → ?T` and `T → ?T` coercions are
> **working**. `must`, `raw`, `fail`, `with`, `catch`, `throw`, and prefix `?`/`!` fallback are
> **spec-only**. See [impl-status.md](impl-status.md).


Error handling in Zith is fully static and return-based — there are no exceptions, and no semicolon is required after `?` or `!`.

### 8.1 Failable Types

| Syntax | Meaning | Propagated by |
|---|---|---|
| `?T` | Optional — `T` or `null`. | `?` (postfix) |
| `T!` | Result — `T` or an error. Equivalent to a `Result<T, E>` where `E` implements `Error`. The compiler infers an anonymous error union when multiple error types are possible. | `!` (postfix) |

Failable types may be stacked, and the notation reads linearly:

```zith
?*?(?i32 ! IoError)
```

Read left to right, outer to inner: an *optional* **pointer** to an *optional* **Result**, where the Result's success type is `?i32` and its error type is `IoError`.

### 8.2 `must` vs. `raw`

| | Debug mode | Release mode |
|---|---|---|
| `must` | Panics with file and line information. | The compiler guides you to remove it, turning it into an `if`/`else` with an early return and a custom error code. |
| `raw` | Always unchecked. | Always unchecked. |

```zith
let cfg: ?Config = tryLoad();
let c1 = must cfg;   // panics in debug; compiler warns/guides in release
let c2 = raw cfg;    // always unchecked; compiler always warns
```

`must` also doubles as an assertion: `must(cond)` panics with file and line info if `cond` is false (debug only). In release, the compiler guides you to replace it with proper error handling.

### 8.3 Propagation & Fallback

```zith
fn readConfig(path: string): Config! {
    let file = File.open(path)!
    let data = file.read()!
    parse(data)!
}

let name = ?user.name or "guest";
let data = !primary() or backup() or default;

// Propagation inside a chain
readFile("data.bin") -> parse(..)! -> validate(..)? -> process(..)
```

Accessing a failable type's inner value requires one of four operators: `?`, `!`, `raw`, or `must`.

- **`?`** unwraps an Optional — returns `T` or `null`.
- **`!`** unwraps a Result — returns `T` or an error.

#### Prefix vs Postfix

`?` and `!` serve two distinct roles depending on position:

| Position | Role | Rule |
|---|---|---|
| **Prefix** (start of expression) | Fallback | Only **one** `?` or `!` per expression. Must be followed by `or` to provide the fallback value. |
| **Postfix** (end of a chain segment) | Propagation | **Multiple** allowed. Propagates the error/null out of the chain, skipping the remaining calls. |

```zith
// Prefix — one per expression, must use 'or' for fallback
let x = ?opt or default;           // valid
let x = !result or fallback;       // valid
let x = ?opt or default or backup; // valid — chain of fallbacks

// Postfix — multiple allowed in a chain
let x = ?y or default.data()?fn()?process()?
//       ^prefix                                        ^postfix (propagation)

// Invalid — two prefixes in one expression
let x = ?y.?data.?fn();   // invalid
let x = ?opt?;            // invalid — '?' cannot follow another '?'
```

You can chain with `or` until it finds an Integral result (short-circuit).

### 8.4 `with` / `catch`

| Form | Behavior |
|---|---|
| `with` | Short-circuit — the first failure jumps straight to `catch` |
| `eager with` | Eager — every expression is evaluated; `catch` runs if any failed |

```zith
// Short-circuit
with (connectDb(), user: getUser(id)) {
    process(user);
}
catch (err) { log(err); }   // any name works; 'err' is convention

// Eager — all expressions run, then catch if any failed
eager with (a: fetchA(), b: fetchB()) {
    use(b);
} catch { log(a, b); }
```

> In `eager with`, all expressions are evaluated before `catch`. The named bindings (`a`, `b`) remain in scope inside `catch` so you can inspect which ones failed. In short-circuit `with`, only the failing expression is known, so `catch` receives a single error parameter.

### 8.5 `fail` Blocks

A `fail` block runs when an error would otherwise escape its associated scope. It can follow a named block (external) or sit inside a block as a scope guard (nameless):

```zith
// External fail
loadConfigure {
    let raw = readFile("config.json")!
    parse(raw)!
} fail loadConfigure(err) {
    if (err is NotFound) { continue(default); }
    throw Error{ context: "load failed", cause: err };
}

// Nameless fail -- guards the current scope
{
    fail (err) { log("scope error:", err); }
    risky()!
    another()!
}
```

> **Name linking:** an external `fail` block's name must match the block it guards. When there is only one failable block in scope, the name can be omitted. A nameless `fail` guards the current scope directly — the compiler passes the error the same way.

Inside a `fail` block, the parameter receives the error directly. You have four options:

- `continue(value)` — resume after the block with a replacement value.
- `return value;` — exit the enclosing function.
- `throw value;` — propagate a new error (requires the `Error` capability).
- Fall through — the original error propagates unchanged.

Use `@ok` to extract the success type from the failure node — useful when `continue` needs to return a value of a different type than the error:

```zith
fail (err) {
    continue(@ok err);   // extract success value from the failure node
}
```

`@err` also exists for extracting the error type in other contexts.

### 8.6 `throw`

```zith
fn divide(a: i32, b: i32): i32! {
    if (b == 0) throw DivisionByZero;
    a / b
}
```

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
