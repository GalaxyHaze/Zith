## 7. Memory Model (NRA)

> **Implementation status:** NRA (Node Resource Analysis) and all ownership modifiers
> (`lend`, `view`, `unique`, `share`, `belong`) are **spec-only** — the analysis pass is not
> implemented. Pointer types (`*T`) are accepted in declarations; `*p` dereference and `&x`
> address-of are **parse errors**. See [impl-status.md](impl-status.md).

### 7.1 What NRA Tracks

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

> For details on how NRA resolves nodes and validates these rules, see [§7.8](#78-how-nra-resolves-nodes).

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

### 7.6 NRA Limitations & Escape Hatches

NRA cannot statically validate every shared or viewed cycle across threads. Three escape hatches cover the remaining cases:

```zith
// await -- safe: the compiler knows the thread is done before the scope ends
let handle = spawn worker(shared_data);
await handle;

// #wont_remain -- a promise that the thread dies before the scope ends
#wont_remain let _ = spawn quick_task(shared_data);

// Rc -- runtime ref-count wrapper, provides thread safety for any type
let shared = Rc.new(HeavyResource.init());
let _ = spawn worker(Rc.clone(shared));
```

> `Rc<T>` and `Arc<T>` are wrappers that provide thread safety for any type `T` — no `Share` requirement on `T` itself. The wrapper handles synchronization internally.

### 7.7 Self-Referential Types

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

### 7.8 How NRA Resolves Nodes

> *This section is relevant for tooling authors and compiler contributors.*

Every symbol gets a **resource node**. NRA is **lazy** — it only validates a node when you use, view, or return it.

#### Node Validation

When you access a node, NRA checks:

1. The node itself is `alive` (not `dead`).
2. Every node in its **dependency vector** — the fields or resources it belongs to or references — is also valid.

If a node is `dead` (say, after a move), NRA records where and why. You get an error pointing right at the violation.

#### Function Evaluation

NRA caches function results. If it has seen a function before, it reuses the cached analysis. Otherwise, it inspects every return path:

- **Every** path returns one of the function's arguments → caller's node is **not consumed** (ownership stays with you).
- **Any** path doesn't return an argument → the result is **consumed**.

Since memory modifiers (`lend`, `view`, `unique`, ...) are in the function signature, NRA knows everything it needs without re-analyzing the body.

#### Branch Isolation (`if` / `else` / `when`)

Each branch runs in isolation. A move inside one branch cannot affect the others. After all branches complete, NRA applies the side effects of whichever branch actually ran.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
