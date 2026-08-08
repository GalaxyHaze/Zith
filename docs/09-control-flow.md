## 9. Control Flow

> **Implementation status:** `if`/`else`, `break`, `continue`, and `return` are **working**.
> `for` is the canonical loop: `for { ... }` and `for (cond) { ... }` are **working** and lower to
> the same CFG as the old `while`. The iterator (`in`) and 3-clause (`init`, `cond`, `step`) forms
> are recognised but report "not implemented yet". `while` still works but emits a deprecation
> warning (`W1008`) pointing at `for (cond) { }`. `when` pattern matching is a **parse error** —
> arm syntax `0 => { }` is not recognised. `flow fn` parses as a function declaration;
> `marker`, `dock`, and `jump target` lowering is tested, while markers with arguments and the
> stackful marker execution model remain future work. See [impl-status.md](impl-status.md).

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
for (i in 0..=9) { @println(i); }               // inclusive range
for (i in 0..9)  { @println(i); }               // exclusive range
for (i = 0), (i < 10), (i += 1) { ... }         // init / cond / step
for (v in range(0, 100)) { @println(v); }       // over a generator

// Destructured group with fallback
let r = for ([acc, i]: i32), (i in 0..n) { acc *= i + 1 } or 0;
```

> If the loop body may never run, its return value is deduced as optional — unless `or` collapses it to a non-optional value.

> The init/cond/step form accepts comma-separated, parenthesized expressions — `for (i = 0), (i < 10), (i += 1)` — or the flat alternative, `for (i = 0, i < 10, i += 1)`.

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
> `jump target` are parsed and lowered. `jump target` is a **restarting transfer**: control
> exits the originating `dock`, runs the target marker, and resumes immediately after that dock
> when the marker body falls through. Markers with arguments are not implemented yet, and
> `stackful marker` currently only carries metadata; its cleaned-up local execution model is
> still future work.

- **`marker`**: A named block of code, hoisted to the top of the `flow fn`. Acts as a label. Receives values via `jump`. Only valid inside a `flow fn`.
- **`dock`**: A block that grants permission to use `jump`. The only accepted form is `dock { ... }`; there is no `dock target;` shortcut. The block itself does not carry arguments.
- **`jump`**: The transfer operator. The simple form transfers control to a target `marker`; it is only valid inside a `flow fn`. When the marker body finishes without an explicit `return`, control resumes at the point after the `dock` that started the current flow. Sending values with the jump is future work.

```zith
flow fn run(data: Stream): void {
    marker Process(chunk: Chunk, count: i32) {
        transform(chunk);
        // count carries over from the last jump unless you update it
    }

    for ( i = 0, item in data ) {
        dock {                          // dock grants jump permission
            if (item.isValid()) {
                jump Process(item, i);  // transfer to marker
            }
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
| Hoisting | **Working.** Marker bodies are collected before lowering the main body and emitted into dedicated HIR blocks. |
| Return point | **Working.** `jump marker` records the continuation of the dock that started the flow. If that marker jumps to another marker, the inner marker still falls through to the original dock, not to the marker that ran it. |
| Marker values | `marker` blocks do not produce values yet. |
| Scope | Future work. Marker argument scoping is not implemented yet. |
| Arguments | Future work. Values passed through `jump` are not implemented yet. |
| Input from dock | `dock` is parsed, typed, and lowered. It opens a block where `jump target;` is permitted; sending values through a dock is future work. |
| Function kind | `marker` and `jump` are restricted to `flow fn`; using them in a regular `fn` reports `E2010`. |
| Global markers | May call regular functions, but not `flow` functions — unless the target is `never`. The `never` exception exists because a `never` flow function never alters the return point — there is no resumption to protect. If it did alter the return point, it would corrupt the state. |

```zith
flow fn foo() {
    marker Test {
        printf("Second\n");
    }
    printf("First\n");
    dock {
        jump Test;
    }
    printf("Third\n");
}
```

The example prints `First`, `Second`, then `Third`: `jump Test` transfers to the marker, and
falling out of `Test` returns to the continuation of the enclosing `dock`.

> **Future work:** `never` markers are not implemented. The restarting transfer description
> assumes markers always resume; a `never` marker would need its own rule and is not accepted
> in this iteration. Per-function flow state, thread-local variables, and marker return values
> are also not implemented yet.

#### Stackful vs Stackless

> **Partial:** the `stackful` modifier is parsed and round-trips through formatting, and the
> lowerer records it, but the cleaned-up local execution model is not implemented yet.

Markers are **stackless** by default — can't create local variables. Opt into **stackful** with the `stackful` modifier. Before the jump, all local variables are cleaned. The following rules apply:

- **Values from outside** (came via `dock`): always valid — the caller owns them.
- **Local values**: never allowed — they would dangle after cleanup.

```zith
flow fn run(data: Stream): void {
    stackful marker Process(chunk: Chunk) {
        let buffer = allocate(chunk.size);  // local — dropped before jump
        jump transform(buffer);
        // only chunk crosses the jump (it came from outside)
    }
}
```

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
