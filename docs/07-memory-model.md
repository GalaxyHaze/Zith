## 7. Memory Model (NRA)

> **Implementation status:** the ownership modifiers (`mut`, `lend`, `view`, `unique`, `share`,
> `belong`) are **parsed and typed** (F-34): they are accepted wherever a type is written, compose
> with `?`, `[]`, and `*`, survive `zithc fmt`, and are interned in the type table, and a write
> through a `view` binding is rejected with `E4004`. The NRA analysis pass itself — the
> alive/dead/lent state machine, fact accumulation, and the four rules of
> [§7.4](#74-the-four-nra-rules) — is still **spec-only** (F-14). The target architecture runs
> `NTA/NRA` before the final HIR boundary; the current implementation still strips qualifiers too
> early during lowering, which is the structural gap F-14 must close. Pointer types (`*T`), `*p`
> dereference, and `&x` address-of are **working**.
> See [impl-status.md](impl-status.md).

### 7.1 What NRA Tracks

The ownership system is split conceptually into two layers:

- `NTA` accumulates semantic facts over a representation that still preserves resource identity,
  qualifier distinctions, captures, escapes, branch facts, and return-path structure.
- `NRA` consumes those facts, applies the four ownership rules, emits diagnostics, and performs
  only the internal canonicalizations that are safe to materialize after the proof boundary.

NRA watches every value in your program and classifies it into one of three states:

| State | Meaning |
|---|---|
| `alive` | Ready to read or use |
| `dead` | Moved away — you cannot read it, only reassign |
| `lent` | Temporarily borrowed — exclusive while the borrow lasts |

It also tracks the **origin** of each node — where the value came from:

| Origin | Example |
|---|---|
| `literal` | `"hello"`, `42` — zero cost, no allocation |
| `allocator` | Heap-allocated via `new` or concatenation |
| `local` | Stack variable |
| `view` | Read-only reference to another node |

With these two axes (state + origin), NRA enforces the rules in [§7.4](#74-the-four-nra-rules).
NTA also records aliasing, branch-local facts, whether a return value is the same node received as
an argument, and whether a `belong` or borrowed value escapes its legal region.

### 7.2 Move Semantics

Moving `a` to `b` redirects the name `b` to `a`'s node. The name `a` is considered **dead** / **invalid** and cannot be read — only reassigned:

```zith
var a = Point { x: 1.0, y: 2.0 };
let b = a;                          // b -> a's node; a becomes dead
// println(a.x);                    -- COMPILE ERROR: a is dead
@println(b.x);                       // OK

a = Point { x: 3.0, y: 4.0 };      // OK: reassignment creates a new node for a
```

In effect, if `a` is never reassigned, it is as though `a` never existed and `b` has held `Point { x: 1.0, y: 2.0 }` all along.

### 7.3 Memory Modifiers

| Modifier | Relationship | Common use |
|---|---|---|
| `default` | Owned. Lifetime follows the binding. | Variables, struct fields |
| `lend` | Exclusive mutable temporary. Cannot be stored, moved, or captured — but **can be returned**, passing the promise to the caller. `belong` fields can also be passed as `lend`. | Passing mutable references to functions |
| `view` | Read-only, non-owning reference. Many views may coexist. | Inspecting without ownership |
| `unique` | Single-owner guarantee — only one name in the graph. | Ownership-transfer patterns |
| `share` | Multiple names, same node, statically validated — no ref-counting. Mutable. | Compile-time-proven sharing |
| `belong` | Part-of relationship. Node lifetime tied to its parent; cannot be stored independently. Can be passed as `lend`. | Back-pointers, hierarchies |

> `unique` provides compile-time single-owner guarantees for local bindings. In a `global` context, `unique` becomes runtime-checked — the compiler enforces exclusive access at program startup. `global` bindings cannot be moved; the `Lent` capability manages thread-safe distribution.

> In practice, most code only needs `lend` and `view`.

#### Implicit Mutability

Each memory modifier carries an implicit content mutability level:

| Modifier | Implies | Example |
|---|---|---|
| `lend` | Mutable | `fn update(p: lend Point) { p.x += 1; }` — `p` is mutable |
| `unique` | Mutable | `let r: unique Resource = ...;` — `r`'s fields are mutable |
| `share` | Mutable | `global counter: share i32 = 0;` — mutable across threads |
| `belong` | Mutable | `parent: ?belong Self` — mutable back-pointer |
| `view` | Immutable | `fn read(c: view Config) { ... }` — `c` is read-only |
| `default` | Depends on `mut` | `let x: Point;` — immutable. `let x: mut Point;` — mutable. |

`default` is the only modifier where mutability is explicitly controlled via the `mut` keyword. All others carry their mutability semantics implicitly.

### 7.4 The Four NRA Rules

**Rule 1 — Argument Exclusivity.** In any call expression, each argument must refer to a distinct node, without exception:
- Duplicating a `default` / `unique` / `lend` argument → **ownership error**.
- Duplicating a `share` / `view` argument → **logic error** (passing the same resource twice is almost certainly a bug).

**Rule 2 — No Dead Node Access.** A symbol cannot be read while its node is `dead`.

**Rule 3 — No Escaping `belong`.** A `belong` node cannot be stored anywhere whose lifetime exceeds any node in its dependency vector. At every use, all of its parents must be `alive`.

**Rule 4 — `lend` Behavioral Promise.** A `lend` value cannot be stored, moved, or captured. It may be passed as a call argument or returned — in the latter case, passing the promise on to the caller.

> For details on how NRA resolves nodes and validates these rules, see [§7.9](#79-how-nra-resolves-nodes).

### 7.5 NRA in Practice

```zith
// lend -- exclusive temporary borrow
fn scale(p: lend Point, factor: f32) { p.x *= factor; p.y *= factor; }
let pt = mut Point { x: 3.0, y: 4.0 };
scale(pt, 2.0);
@println(pt.x);   // OK: borrow ended

// view -- multiple read-only refs
let v1: view Point = pt;
let v2: view Point = pt;   // fine

// share -- no ref-count, statically proven
let a: share Config = load();
let b: share Config = a;   // both point to the same node

// belong -- back-pointer cannot outlive its parent
struct Tree<T> {
    data:     T,
    children: []unique Self,
    parent:   ?belong Self,
}

// belong fields can be passed as lend
fn getParent(self: view Node): lend Node { self.parent }
```

### 7.6 Boundary Before HIR

The main NRA proof runs before the final HIR is formed. That boundary exists so the analysis still
sees:

- binding identity and resource graphs;
- the difference between `default`, `view`, `lend`, `unique`, `share`, and `belong`;
- branch facts, narrowing facts, and return-path equivalence;
- call, capture, and escape structure before lowering erases it.

The final HIR is therefore not the place where ownership is re-proven. It receives a typed,
desugared, NRA-validated view of the program plus only the residual facts that still matter for
lowering, cache serialization, and backend hints.

### 7.7 Residual Facts and Internal Canonicalization

NRA may materialize limited internal canonicalizations after it has proven the ownership contract,
but those rewrites do not change a public signature or observable ABI. For example, forwarding a
proven move internally or removing a temporary introduced only to preserve ownership is valid;
redefining an exported function's calling convention is not.

Residual facts that may survive into HIR include:

- consumed vs. non-consumed value state when lowering depends on it;
- non-null or otherwise narrowed facts that affect control-flow lowering;
- borrow, capture, or escape decisions that codegen and caching must preserve;
- internal calling-convention details only when they stay behind a stable boundary.

LLVM is not the source of truth for ownership. At most it receives hints already decided by NRA,
such as `nonnull`, `noalias`, `readonly`, `nocapture`, or opportunities to remove redundant
temporaries and stores.

### 7.8 Self-Referential Types

```zith
struct Node<T> {
    data: T,
    next: ?unique Self,
    prev: ?belong Self,
}

implement Node<T> {
    fn append(self: lend Self, data: T) {
        self.next = unique Node { data, next: null, prev: belong self };
    }
}
```

- Freeing the head frees the entire chain, since `next` forms a `unique` ownership chain.
- NRA guarantees `prev` (`belong`) never outlives its owner.
- `belong` fields may be passed as `lend` to functions.

### 7.9 How NRA Resolves Nodes

> *This section is relevant for tooling authors and compiler contributors.*

Every symbol gets a **resource node** before final HIR lowering. NTA and NRA are lazy in the sense
that they validate nodes when use, view, move, return, capture, or escape facts make the proof
relevant.

#### Node Validation

When you access a node, NRA checks:

1. The node itself is `alive` (not `dead`).
2. Every node in its **dependency vector** — the fields or resources it belongs to or references — is also valid.

If a node is `dead` (say, after a move), NRA records where and why. You get an error pointing right at the violation.

#### Function Evaluation

NRA caches function results. If it has seen a function before, it reuses the cached analysis.
Otherwise, it inspects every return path:

- **Every** path returns one of the function's arguments → caller's node is **not consumed**
  (ownership stays with you).
- **Any** path doesn't return an argument → the result is **consumed**.

Those return facts are preserved into HIR only in residual form. HIR should not have to rediscover
which node a return came from.

#### Branch Isolation (`if` / `else` / `when`)

Each branch runs in isolation. A move inside one branch cannot affect the others. After all branches
complete, NRA applies the side effects of whichever branch actually ran and emits only the merged
facts that lowering still needs.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
