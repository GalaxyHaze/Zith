## 9. Control Flow

> **Status:** see [impl-status.md](impl-status.md) for the current status of each control-flow
> form. `for` is the canonical loop and `while` still works but emits a deprecation warning
> (`W1008`). The legacy `~>` `when` marker still compiles but emits `W1008`, and the old
> `flow fn`/`marker`/TLS-blob model is removed.

### 9.1 Syntax Rules

Parentheses `()` are required around every control expression except when it
starts with a function call. For compatibility, `not expr` may also omit the
outer parentheses in `if`, `while`, `for`, and conditional `else`. The
condition-less infinite loop `for { ... }` needs no parentheses. A missing pair
is reported as `E1011`, with a correction showing where to add it. Logical
operators use English keywords. Bitwise operators use standard symbols
followed by `.`:

```zith
if (x > 0 and y < 10) { ... }
if isTrue() and (x > 5) { ... }
let mask = a &. b |. c ^. d;
```

Boolean negation is written as `not`. `!` is not accepted as a prefix unary
operator in this iteration and remains reserved for a future postfix form:

```zith
if (not (x > 0)) { ... }
```

### 9.2 `for`

```zith
for { ... }                                     // infinite
for (i in iterable) { @println(i); }            // legacy Zith-- iterator protocol: next(self): ?T
for (i = 0), (i < 10), (i += 1) { ... }         // init / cond / step
for (v in range(0, 100)) { @println(v); }       // over a generator

// Destructured group with fallback
let r = for ([acc, i]: i32), (i in 0..n) { acc *= i + 1 } or 0;
```

Labels use `name: for ...` / `name: while ...` and are stored on the loop
expression. `break name;` and `continue name;` target that loop. Without a
label they target the innermost active loop. An unknown or duplicate active
label is rejected by semantic analysis. Labeled exits run cleanup for every
loop between the current innermost loop and the target:

```zith
outer: for (...) {                               // labeled loop
    for (...) {
        break outer;                             // exits the outer loop
        continue outer;                          // continues the outer loop
    }
}
```

> If the loop body may never run, its return value may be invalid. `or` can provide a fallback.

> The init/cond/step form accepts comma-separated, parenthesized expressions, such as `for (i = 0), (i < 10), (i += 1)`, or the flat alternative `for (i = 0, i < 10, i += 1)`. The current Zith-- compiler accepts an iterator protocol whose `next(self)` returns legacy `?T`, with `null` ending iteration. The full-Zith iterator result protocol has not yet been specified.

```zith
struct Range {
    current: i32,
    limit: i32,
    fn next(var self): ?i32 {
        if (self->current >= self->limit) {
            return null;
        }
        let value = self->current;
        self->current = self->current + 1;
        return value;
    }
}

fn main() {
    let range = Range { current: 0, limit: 3 };
    for (x in range) {
        println(x);
    }
}
```

The tagged-union `{ T, End }` protocol remains accepted during migration, but the optional
protocol is canonical and should be used for new iterators.

### 9.3 Pipeline (`|>` and `also`)

Pipelines are C-style and explicitly threaded. Each stage reads the current
value through `it`; there is no automatic injection, implicit argument, or
tag/`?`/`!` propagation. `x |> f(it)` is equivalent to `f(x)` except that the
source value is materialized once before the stage runs. A `|>` stage must
reference the current value exactly once on every execution path.

`also` is a side-effect stage: it may observe `it` zero or more times, keeps
the chain value unchanged, and is valid only as a chain operator. An
observation may only pass `it` to a read-only `view` parameter, and the borrow
cannot escape the stage: the borrowed value, aliases, and derived references
stay inside the stage, while independent copies of fields or computed values
may escape. `it` is valid only inside a pipeline stage.

`|>` replaces the chain value with the stage result. `also` performs a
side-effect stage and keeps the chain value unchanged, so `x also f(view it)`
returns `x`. `also` is only a chain operator, not a standalone statement. Both operators
are left-associative and have lower precedence than calls; do not use `->` for
this role.

```zith
getData() |> process(it) |> save(it);

getData()
    |> parse(it)
    |> validate(it)
    |> save(it);

// Inline side-effect: log the current value without advancing the chain.
readFile("data.bin")
    also log(view it)
    |> process_body(it);

// A block effect stage can bind locals, but must not transfer control.
readFile("data.bin")
    also { let header = parse_header(view it); validate(header); }
    |> process_body(it);
```

An `also` block discards its final value: the chain keeps the source value.
`return`, `jump`, and `break`/`continue` that would exit the stage are
rejected; `break`/`continue` aimed at loops declared inside the stage are
allowed.

### 9.4 `state` Functions & State Machines

A `state` function declares one state in a machine. `dock` starts a machine and evaluates to
its eventual return value. `jump` terminates the current state and tail-calls the next state.
Every state in one machine shares the same return type, while parameter lists may differ
between states. The return type drives machine grouping. Each transition validates arity and
argument types against its individual target. LLVM `tailcc` plus `musttail` lets transitions
between different signatures still compile to direct, stackless calls.

- **`state`**: `state Name(params): ReturnType { ... }`. Each state is a normal function body
  and may use module/global state and ordinary locals.
- **`dock`**: `dock Start(args)` is an expression. It calls a state and returns the value
  produced by `return` in the final state.
- **`state(params): ReturnType` value**: a real state declaration can be stored in a value
  with the matching `state(params): ReturnType` signature, and `dock` accepts that value with
  the same argument and return-type checks as a direct state call.
- **`jump`**: `jump Next(args);` is terminating and only valid inside a state. It may target a
  state with a different parameter list from the same machine, and it lowers to `musttail tailcc`
  followed by `ret`.
- **`return`**: `return value;` inside a state completes the machine and supplies the result
  to the originating `dock` call.

```zith
state Count(n: i32): i32 {
    if (n == 0) {
        return n;
    }
    jump Count(n - 1);
}

fn main(): i32 {
    let result = dock Count(3);
    return result;
}
```

Transitions never grow the stack on targets with backend `musttail` support. All states in a
machine use LLVM `tailcc`, including ordinary `dock` calls into them, so transitions between
different parameter lists keep one consistent ABI. The compiler diagnoses unsupported targets
instead of silently falling back to a marker runtime.

```zith
state Machine(n: i32): i32 {
    return n;
}

fn main(): i32 {
    let S: state(i32): i32 = Machine;
    return dock S(42);
}
```

### 9.5 Scope Cleanup (`defer`)

`defer expr;` and `defer { ... }` are implemented in Zith--.
`defer expr;` registers one cleanup expression. `defer { ... }` registers the
whole body as a cleanup-only block that does not produce a value. Statements in
a deferred body run in written order, while separate `defer` statements run in
reverse registration order when their nearest lexical block exits.

A `defer` may capture bindings declared later in the same lexical block. The
sema defers typing of the deferred body until the block's direct bindings are
known, and lowering emits deferred cleanup after those slots are created. If a
`return`, `break`, `continue`, `jump`, or nested control-flow exit before the
binding can leave the block while the capture is still uninitialized, the
compiler rejects it:

```zith
fn main(): i32 {
    defer cleanup(v);   // ok when `v` is initialized before every exit
    var v: i32 = 1;
    return 0;
}
```

- `defer expr;` registers a cleanup expression for the enclosing block.
- Expressions run when the block exits, in reverse registration order.
- They run on normal fallthrough and across `return`, `break`, `continue`, and
  `state` `jump` transitions.
- `return value;` evaluates the value first. Deferred bodies run immediately
  before the terminator.
- A `defer` body may not itself contain `return`, `break`, `continue`, or
  `jump`.

In Zith--, a `defer` emitted by a `raw macro` registers with the lexical block
containing the macro call. The expansion does not create a cleanup scope.
Repeated statements inside that deferred body run in argument order, while
separate defer registrations keep reverse registration order.

Inside a loop, cleanup runs each time the loop body exits, including a
`break` or `continue`. For Zith--, `drop` remains a reserved keyword but
automatic per-owner cleanup is not planned. Cleanup stays explicit. ADR-0055
records the accepted ownership design, and
[`defer-drop.md`](plans/defer-drop.md) preserves the superseded proposal.

### 9.6 Named Blocks and Scope Markers

A statement in a `fn` or `state` body may carry a descriptive name:

```zith
fn main() {
    setup: {
        // ordinary lexical scope, name is descriptive only
    }
}
```

The plain named block creates a lexical scope. The name has no runtime effect,
introduces no cleanup, and is never a `break` or `continue` target.

A named block may also carry a scope marker, a call to a macro declared with
`#[scope]`:

```zith
render: @scoped(renderer, log) {
    renderer.draw();
    log.write("frame");
}
```

Each marker argument must be a place-expression (a binding, a parameter, a
global, or a field) whose type implements `Scope`. A temporary is rejected.
The compiler expands the marker into one hidden `enter` per argument, left to
right, and registers the matching `leave` in reverse order through `defer`, so
the cleanup runs on every exit from the block, including `return`. The guard
bindings are hidden and hygienic. `enter` and `leave` do not fail at the
language surface.

The standard library declares the traits and the marker macro:

```zith
pub trait Guard {
    fn leave(var self): void;
}

pub trait Scope {
    fn enter(var self): dyn Guard;
}

#[scope]
pub macro scoped(resources: identifier...) { }
```

An implementation chooses its own guard type and the effect `leave` performs.
The `#[scope]` attribute is the contract: only a macro carrying it may appear
in the outer position `name: @macro(...) { }`, and no macro name is hardcoded
in the compiler. The spec names the macro `marker`, but `marker` is a reserved
keyword, so the stdlib macro is `scoped`.

The marker argument stays available to the block body only when the `Scope`
implementation borrows its receiver. An implementation whose `enter` takes the
receiver by value (a bare `self` or `var self`) moves the caller binding, so a
later read of that argument reports `E4001 UseAfterMove`. Write
`fn enter(self: view Self): dyn Guard` (or `self: lend Self`) when the body
needs to keep using the resource, as in the `render: @scoped(renderer, log)`
example above.

Position is restricted. A named block is valid only as a direct statement of a
`fn` or `state` body. It is rejected inside `if`, loops, anonymous blocks, and
other named blocks. The one composed position is a pipeline stage, where the
named block is a normal stage with every pipeline rule intact:

```zith
value
    |> step1: { it + 1 }
    |> render: @scoped(r) { it * 2 };
```

Names are unique per `fn`/`state` body. A duplicate name, including a collision
with a loop label in the same body, is rejected. Names are preserved in the HIR
for tooling and diagnostics but are not part of the ABI or the ZIRL format.

Three dedicated diagnostics cover the new rejections: a named block in an
invalid position, a marker argument that is not a place or does not implement
`Scope`, and a duplicate name. Any other diagnostic raised while the body of a
named block is checked is prefixed with `in block 'name':`, so an error deep in
a phase points at the phase without the reader having to reconstruct which
block it belongs to.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
