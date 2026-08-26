## 9. Control Flow

> **Implementation status:** `if`/`else`, `break`, `continue`, and `return` are **working**.
> `for` is the canonical loop: `for { ... }`, `for (cond) { ... }`, the comma-based 3-clause form,
> and duck-typed `for (x in iterable)` are **working** and lower to the same CFG machinery as the
> old `while`. `while` still works but emits a deprecation warning (`W1008`) pointing at
> `for (cond) { }`. The literal range forms (`0..4`) are not implemented yet. `when` pattern
> matching is **working**, including equality, boolean, range, and tagged-union type-narrowing
> arms. `state` declarations, `dock` calls, and `jump` terminating transfers are **working**
> and compile to direct LLVM `musttail` calls; the old `flow fn`/`marker`/TLS-blob model is
> removed. See [impl-status.md](impl-status.md).

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
for (i in iterable) { @println(i); }            // user iterator with next() and End
for (i = 0), (i < 10), (i += 1) { ... }         // init / cond / step
for (v in range(0, 100)) { @println(v); }       // over a generator

// Destructured group with fallback
let r = for ([acc, i]: i32), (i in 0..n) { acc *= i + 1 } or 0;
```

> If the loop body may never run, its return value is deduced as optional — unless `or` collapses it to a non-optional value.

> The init/cond/step form accepts comma-separated, parenthesized expressions — `for (i = 0), (i < 10), (i += 1)` — or the flat alternative, `for (i = 0, i < 10, i += 1)`. The iterator form expects a value whose type exposes `next(self)`. `next` must return a tagged union with exactly two members: the element type and the empty `End` marker. `for` calls `next` once per iteration, exits when the result is `End`, and otherwise binds the non-End member as the loop variable.

```zith
struct End {}

union RangeStep { i32, End }

struct Range {
    current: i32,
    limit: i32,
    fn next(var self): RangeStep {
        if (self->current >= self->limit) {
            return RangeStep { End {} };
        }
        let value = RangeStep { self->current };
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

### 9.4 `state` Functions & State Machines

A `state` function declares one state in a machine. `dock` starts a machine and evaluates to
its eventual return value; `jump` terminates the current state and tail-calls the next state.
Every state in one machine shares the same return type, while parameter lists may differ
between states. The return type drives machine grouping; each transition validates arity and
argument types against its individual target. LLVM `tailcc` plus `musttail` lets transitions
between different signatures still compile to direct, stackless calls.

- **`state`**: `state Name(params): ReturnType { ... }`. Each state is a normal function body
  and may use module/global state and ordinary locals.
- **`dock`**: `dock Start(args)` is an expression. It calls a state and returns the value
  produced by `return` in the final state.
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

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
