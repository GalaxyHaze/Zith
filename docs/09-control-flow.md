## 9. Control Flow

> **Implementation status:** `if`/`else`, `break`, `continue`, and `return` are **working**.
> `for` is the canonical loop: `for { ... }`, `for (cond) { ... }`, the comma-based 3-clause form,
> and duck-typed `for (x in iterable)` are **working** and lower to the same CFG machinery as the
> old `while`. `while` still works but emits a deprecation warning (`W1008`) pointing at
> `for (cond) { }`. The literal range forms (`0..4`) are not implemented yet. `when` pattern
> matching is a **parse error** —
> arm syntax `0 => { }` is not recognised. `flow fn` parses as a function declaration;
> `dock`, `jump`, and global/local `marker` lowering with typed arguments is tested, along with
> stackless marker execution and stackful local bindings. Cycle detection and marker return
> values remain future work. See [impl-status.md](impl-status.md).

### 9.1 Syntax Rules

Parentheses `()` are mandatory on every control structure's condition except function calls. Logical operators use English keywords; bitwise operators use standard symbols followed by `.`:

```zith
if (x > 0 and y < 10) { ... }
if isTrue() and (x > 5) { ... }
let mask = a &. b |. c ^. d;
```

### 9.2 `for`

```zith
for { ... }                                     // infinite
for (i in iterable) { @println(i); }            // user iterator with done/value/next
for (i = 0), (i < 10), (i += 1) { ... }         // init / cond / step
for (v in range(0, 100)) { @println(v); }       // over a generator

// Destructured group with fallback
let r = for ([acc, i]: i32), (i in 0..n) { acc *= i + 1 } or 0;
```

> If the loop body may never run, its return value is deduced as optional — unless `or` collapses it to a non-optional value.

> The init/cond/step form accepts comma-separated, parenthesized expressions — `for (i = 0), (i < 10), (i += 1)` — or the flat alternative, `for (i = 0, i < 10, i += 1)`. The iterator form expects a value whose type exposes `done(self): bool`, `value(self): T`, and `next(self)` methods.

### 9.3 Chain Flow (`->`)

The `->` operator pipes output left to right. The previous value is available as `..`, and tags capture values for later use. `!` and `?` propagate out of the chain normally. Precedence is left-to-right and lower than function calls.

```zith
getData() -> process(..) -> save(..);

getData()
    -> raw:    parse(..)
    -> parsed: validate(..)!       // ! propagates out of the chain
    -> connectDb()
    -> save(parsed);

// Inline block
readFile("data.bin")
    -> { let h = parse_header(..); validate(h)! }
    -> process_body(..);

// Comma sub-chain -- f1 and f2 receive foo's value but do NOT advance the chain
foo(), f1(..), f2(..) -> f3(..);

// Parenthesized sub-chain -- this one does advance inside the sub-chain
// But don't affect the main chain
foo(), ( f1(..) -> f2() ) -> f3(..);
         ^                      ^
         |                      |
         foo                    foo
```

> Comma sub-chains are useful for side effects — logging, validation — without disrupting the main data flow.

### 9.4 `flow` Functions & Markers

A `flow fn` lets you write control flow using **markers**, **docks**, and **jumps**:

> The code below documents the full language contract. Today `marker`, `dock`, and
> `jump target(...)` are parsed, typed, and lowered. `jump` is a **restarting transfer**: control
> starts at the target marker, stores its arguments in the module-local marker blob, and resumes
> immediately after the originating `dock` when the marker body falls through. Stackless markers
> cannot capture bindings from the host flow function; stackful markers may use their own local
> bindings.

- **`marker`**: A named block of code with typed parameters. Global markers are module-scoped and usable from any `flow fn` in the same module; local markers may shadow a global within their `flow fn`. Marker bodies run from a shared sample inside each host flow function. Markers are `void`; there is no marker return value yet.
- **`dock`**: A statement that grants permission to use `jump` and records the continuation of the enclosing `flow fn`. The accepted form is `dock target(args);`; the old `dock { ... }` block form is rejected. Docks are only valid inside a `flow fn` and cannot appear inside a marker body.
- **`jump`**: The transfer operator: `jump target(args);` is only valid inside a marker. It stores new argument values into the module-local TLS marker blob and transfers to the target marker without changing the continuation. When the outermost marker body finishes without an explicit `return`, control resumes at the point after the `dock` that started the flow.

```zith
flow fn run(data: Stream): void {
    marker Process(chunk: Chunk, count: i32) {
        transform(chunk);
        // count carries over from the last jump unless you update it
    }

    for ( i = 0, item in data ) {
        if (item.isValid()) {
            dock Process(item, i);      // start marker flow with arguments
        }
        i += 1;
    }
}

// Global marker — usable from any flow fn
marker ContextSwitch(next: TaskId) {
    saveRegisters();
    loadTask(next);
}

// never: the return point is not altered — no resumption to protect
flow fn scheduler(): never { ... }
```

#### Marker Rules

| Rule | Detail |
|---|---|
| Hoisting | **Working.** Markers are registered module-scope and their bodies are lowered as shared samples into each reachable host `flow fn`. |
| Return point | **Working.** `dock` records the continuation of its enclosing flow. If a marker jumps to another marker, the inner marker still falls through to the original dock, not to the marker that ran it. |
| Marker values | Markers are `void`; marker return values are not implemented yet. |
| Scope | **Working.** Global markers are module-scoped; local markers shadow globals within their `flow fn`. Marker bodies do not see host parameters or locals. |
| Arguments | **Working.** Markers have typed parameters. `dock target(args)` and `jump target(args)` validate arity and argument types. Stackless markers cannot capture host locals. |
| Input from dock | `dock target(args);` is parsed, typed, lowered, and materialized into the module-local TLS marker blob. |
| Function kind | `marker`, `dock`, and `jump` are restricted to `flow fn` context; using them elsewhere reports the appropriate semantic error. `dock` outside a `flow fn`, `dock` inside a marker, and `jump` outside a marker are rejected. |
| Global markers | May call regular functions, but not `flow` functions. Marker bodies root in module scope and are shared across flow functions that dock or jump to them. |

```zith
flow fn foo() {
    marker Test() {
        printf("Second\n");
    }
    printf("First\n");
    dock Test();
    printf("Third\n");
}
```

The example prints `First`, `Second`, then `Third`: `jump Test` transfers to the marker, and
falling out of `Test` returns to the continuation of the enclosing `dock`.

> **Future work:** `never` markers are not implemented. The restarting transfer description
> assumes markers always resume; a `never` marker would need its own rule and is not accepted
> in this iteration. Marker cycle detection and marker return values are also not implemented
> yet.

#### Stackful vs Stackless

A `stackful marker` may declare and use its own local bindings. A **stackless** marker cannot
declare local bindings or capture bindings from the host flow function. Stackless markers
therefore only touch their parameters and module/global state; stackful markers may also use
ordinary stack allocation inside the marker body.

```zith
flow fn run(data: Stream): void {
    stackful marker Process(chunk: Chunk) {
        let buffer = allocate(chunk.size);  // local — dropped before jump
        jump transform(buffer);
        // chunk and buffer cross the jump only within this marker's own frame
    }
}
```

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
