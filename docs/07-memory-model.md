## 7. Memory Model (NRA)

> **Document status:** this chapter describes the legacy Zith-- ownership
> surface (`lend`, `view`, `share`, `own`, `belong`) and the earlier
> full-Zith draft built on it. Zith-- implements only a slice of it, the
> `lend`/`view` call-annotation analysis, which is now called SRA (Small
> Resource Analysis) and is frozen: it will not grow into the full proof.
> The current full-Zith model is the rewritten
> [nra-spec.md](nra-spec.md), which supersedes sections 7.1 through 7.7
> wherever they disagree.
>
> **Status:** see [impl-status.md](impl-status.md) for the current status of the ownership slice.
> Zith-- implements the `lend`/`view` call-annotation slice (now SRA, Small Resource Analysis):
> `lend T`/`view T` parameters lower to real pointers, call arguments for `default` bindings
> require `lend x`/`view x` (`E4005`), invalid call annotations are rejected (`E4007`), and a
> binding cannot be borrowed twice in one call. The full proof machine (node state, edge state,
> flow, and rules NRA-1..NRA-11) is full-Zith work specified in [nra-spec.md](nra-spec.md).

The implemented Zith-- behavior above is separate from the full-Zith reference
direction. [ADR-0033](adr/0033-nra-reference-model-and-bind.md) defines `&T`,
`&mut T`, `^T`, and `%T`; [the future NRA spec](nra-spec.md) is its source of
truth. Sections 7.3-7.5 below preserve an earlier full-Zith draft and are not
the current full-Zith surface contract. The self-referential example in §7.8
also uses that earlier surface.

### 7.1 What NRA Tracks

The ownership system is split conceptually into two layers:

- `NIA` accumulates numeric facts (ranges, versions, control-flow joins) over a representation
  that still preserves resource identity.
- `NRA` consumes those numeric facts together with the residual ownership facts (resource
  identity, qualifier distinctions, captures, escapes, branch facts, and return-path structure),
  applies the ownership rules, emits diagnostics, and performs only the internal
  canonicalizations that are safe to materialize after the proof boundary.

NRA watches every value in your program and classifies it into one of three states:

| State | Meaning |
|---|---|
| `alive` | Ready to read or use |
| `dead` | Moved away — you cannot read it, only reassign |
| `lent` | Temporarily borrowed — exclusive while the borrow lasts |

> These three state names are the legacy draft vocabulary. The current full-Zith model
> ([nra-spec.md](nra-spec.md)) uses `uninitialized` / `taken` / `ok` and enumerates rules
> NRA-1..NRA-11; it supersedes this table wherever they disagree.

It also tracks the **origin** of each node, which says where the value came from:

| Origin | Example |
|---|---|
| `literal` | `"hello"`, `42` — zero cost, no allocation |
| `allocator` | Heap-allocated via `new` or concatenation |
| `local` | Stack variable |
| `view` | Read-only reference to another node |

With these two axes (state + origin), NRA enforces the rules in [§7.4](#74-the-four-nra-rules).
NRA also records aliasing, branch-local facts, whether a return value is the same node received as
an argument, and whether a `belong` or borrowed value escapes its legal region.

### 7.2 Move Semantics

Moving `a` to `b` redirects the name `b` to `a`'s node. The name `a` is considered **dead** / **invalid** and cannot be read, only reassigned:

```zith
var a = Point { x: 1.0, y: 2.0 };
let b = a;                          // b -> a's node; a becomes dead
// println(a.x);                    -- COMPILE ERROR: a is dead
@println(b.x);                       // OK

a = Point { x: 3.0, y: 4.0 };      // OK: reassignment creates a new node for a
```

In effect, if `a` is never reassigned, it is as though `a` never existed and `b` has held `Point { x: 1.0, y: 2.0 }` all along.

### 7.2.1 Pointer Objects in Zith--

In Zith--, `*T` is a non-nullable pointer object and `?*T` is the same object
nullable. `p.x`, `p->x` and `*p` remain valid accesses. `&x` performs a logical move of the
binding: reads of `x` after it report `E4001 UseAfterMove`, while a direct assignment to `x`
revives a new local version. There is no SSA/phi infrastructure. The version is a per-binding
counter used by sema.

Pointers derived from `&x` or `@ptrOf(local)` are escaping values in this iteration. Returning
one, storing it in a struct/array/global/deferred aggregate, or otherwise letting it outlive
the storage scope reports `E4008 PointerEscapesScope`. `raw` index/deref remains the unchecked
escape and does not create pointer-object aliasing.

### 7.3 Memory Modifiers

| Modifier | Relationship | Common use |
|---|---|---|
| `default` | Owned. Lifetime follows the binding. | Variables, struct fields |
| `lend` | Exclusive mutable temporary. Cannot be stored, moved, or captured — but **can be returned**, passing the promise to the caller. `belong` fields can also be passed as `lend`. | Passing mutable references to functions |
| `view` | Read-only, non-owning reference. Many views may coexist. | Inspecting without ownership |
| `own` | Single-owner guarantee — only one name in the graph. | Ownership-transfer patterns |
| `share` | Multiple names, same node, statically validated — no ref-counting. Mutable. | Compile-time-proven sharing |
| `belong` | Part-of relationship. Node lifetime tied to its parent; cannot be stored independently. Can be passed as `lend`. | Back-pointers, hierarchies |

> `own` provides compile-time single-owner guarantees for local bindings. In a `global` context, `own` becomes runtime-checked. The compiler enforces exclusive access at program startup. `global` bindings cannot be moved, and the `Lent` capability manages thread-safe distribution.

`own T` is the full-Zith surface name for the former `unique T`. The NRA
semantic stays a mutable single-owner handle. A stack-backed handle may not
escape its storage scope.

> In practice, most code only needs `lend` and `view`.

`share` nodes may participate in the thread protocol. When a shared value is
handed to a thread through `backend fork Entry(share value)`, NRA tracks a
`forkCount`: zero means the source owns the node, one means a thread branch is
pending, and more than one is rejected. `merge` consumes the thread handle and
returns the entry result. It does not reclaim the `share` value. See [the branch
protocol plan](plans/branch-protocol.md).

#### Implicit Mutability

Each memory modifier carries an implicit content mutability level:

| Modifier | Implies | Example |
|---|---|---|
| `lend` | Mutable | `fn update(p: lend Point) { p.x += 1; }` — `p` is mutable |
| `own` | Mutable | `let r: own Resource = ...;` — `r`'s fields are mutable |
| `share` | Mutable | `global counter: share i32 = 0;` — mutable across threads |
| `belong` | Mutable | `parent: ?belong Self` — mutable back-pointer |
| `view` | Immutable | `fn read(c: view Config) { ... }` — `c` is read-only |
| `default` | Depends on `mut` | `let x: Point;` — immutable. `let x: mut Point;` — mutable. |

`default` is the only modifier where mutability is explicitly controlled via the `mut` keyword. All others carry their mutability semantics implicitly.

`view` remains read-only for every field, including a struct field declared
`var`. A field qualifier does not permit interior mutation through a `view`.
It also does not add synchronization or relax cross-thread safety requirements.

### 7.3.1 Re-binding With `:=`

`=` assigns values; `:=` re-binds the reference or ownership slot itself. This
is the operator for retargeting an existing `own`, `share`, `lend`, `belong`,
or `view` link, and for installing an owner into a slot that is awaiting one.
Plain `=` is allowed only when the target is alive and the operation is a value
copy or a value store.

```zith
var slot: own Buffer = acquireBuffer();  // OK: initialization uses `=`
slot := acquireBuffer();      // ERROR: first owner is still live
let slot2 = &slot;            // slot2 owns the storage; slot is dead
slot := acquireBuffer();      // OK: install into the dead slot
```

`:=` never overwrites a live owner. For `lend`, the source node must be alive
when the borrow starts and the previous borrow must end before the new one
starts. For `belong`, the new parent must still satisfy the normal escape and
parent-alive rules. For `view` and `share`, it only changes which node the
link points at; it does not create or remove ownership.

This is full-Zith surface. `Zith--` keeps its current reassignment rules and
does not introduce `:=`.

### 7.4 The Four NRA Rules

**Rule 1: Argument Exclusivity.** In any call expression, each argument must refer to a distinct node, without exception:
- Duplicating a `default` / `own` / `lend` argument → **ownership error**.
- Duplicating a `share` / `view` argument → **logic error** (passing the same resource twice is almost certainly a bug).

**Rule 2: No Dead Node Access.** A symbol cannot be read while its node is `dead`.

**Rule 3: No Escaping `belong`.** A `belong` node cannot be stored anywhere whose lifetime exceeds any node in its dependency vector. At every use, all of its parents must be `alive`.

**Rule 4: `lend` Behavioral Promise.** A `lend` value cannot be stored, moved, or captured. It may be passed as a call argument or returned, in which case it passes the promise on to the caller.

> For details on how NRA resolves nodes and validates these rules, see [§7.9](#79-how-nra-resolves-nodes).

### 7.5 NRA in Practice

```zith
// lend -- exclusive temporary borrow
fn scale(p: lend Point, factor: f32) { p.x *= factor; p.y *= factor; }
var pt: Point = Point { x: 3.0, y: 4.0 };
scale(lend pt, 2.0);
@println(pt.x);   // OK: borrow ended

// view -- read-only borrow
fn center_of(p: view Point): f64 { (p.x + p.y) / 2.0 }
center_of(view pt);   // no caller mutation

// share/belong/own are full-Zith forms, outside the Zith-- slice

// belong -- back-pointer cannot outlive its parent
struct Tree<T> {
    data:     T,
    children: []own Self,
    parent:   ?belong Self,
}

// belong fields can be passed as lend
fn getParent(self: view Node): lend Node { self.parent }
```

### 7.6 Boundary Before HIR

The main NRA proof runs before the final HIR is formed. That boundary exists so the analysis still
sees:

- binding identity and resource graphs
- the difference between `default`, `view`, `lend`, `own`, `share`, and `belong`
- branch facts, narrowing facts, and return-path equivalence
- call, capture, and escape structure before lowering erases it.

The final HIR is therefore not the place where ownership is re-proven. It receives a typed,
desugared, NRA-validated view of the program plus only the residual facts that still matter for
lowering, cache serialization, and backend hints.

### 7.7 Residual Facts and Internal Canonicalization

NRA may materialize limited internal canonicalizations after it has proven the ownership contract,
but those rewrites do not change a public signature or observable ABI. For example, forwarding a
proven move internally or removing a temporary introduced only to preserve ownership is valid,
redefining an exported function's calling convention is not.

Residual facts that may survive into HIR include:

- consumed vs. non-consumed value state when lowering depends on it
- non-null or otherwise narrowed facts that affect control-flow lowering
- borrow, capture, or escape decisions that codegen and caching must preserve
- internal calling-convention details only when they stay behind a stable boundary.

LLVM is not the source of truth for ownership. At most it receives hints already decided by NRA,
such as `nonnull`, `noalias`, `readonly`, `nocapture`, or opportunities to remove redundant
temporaries and stores.

### 7.8 Self-Referential Types

```zith
struct Node<T> {
    data: T,
    next: ?own Self,
    prev: ?belong Self,
}

implement Node<T> {
    fn append(self: lend Self, data: T) {
        self.next := own Node { data, next: null, prev: belong self };
    }
}
```

- Freeing the head frees the entire chain, since `next` forms an `own` ownership chain.
- NRA guarantees `prev` (`belong`) never outlives its owner.
- `belong` fields may be passed as `lend` to functions.

### 7.9 How NRA Resolves Nodes

> *This section is relevant for tooling authors and compiler contributors.*

Every symbol gets a **resource node** before final HIR lowering. NIA and NRA are lazy in the sense
that they validate nodes when use, view, move, return, capture, or escape facts make the proof
relevant.

#### Node Validation

When you access a node, NRA checks:

1. The node itself is `alive` (not `dead`).
2. Every node in its **dependency vector**, which includes the fields or resources it belongs to or references, is also valid.

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
