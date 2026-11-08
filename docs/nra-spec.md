# NRA Spec - Node Resource Analysis

Status: draft. This document is the source of truth for the full-Zith NRA
design. It replaces the earlier draft of this file, which mixed the old
`lend`/`view`/`share`/`belong` surface with the reference model of ADR-0033.
`docs/impl-status.md` describes what the current Zith-- toolchain implements,
which is a separate, smaller analysis called SRA (Small Resource Analysis).

NRA is one of the four Zith Proof Kernel (ZPK) sub-systems (see
`docs/proof-model.md`). It proves ownership and lifetime safety before the
final HIR is formed. It never re-proves ownership from HIR or LLVM.

## 1. Scope and Position

NRA answers one question: who may use this resource, when, and for how long.
It consumes numeric facts from NIA, geometric facts from RRA, and region
identity and permissions from MRA. It does not reimplement their domains.

NRA reads a derived effect header for each function and applies the declared
effects to resource nodes at call sites. When a function has a cached
effect header, the call site uses that header. NRA does not scan the complete
callee body again for normal calls.

The proof is lazy where possible: facts are collected, grouped, and then
resolved. Invalidation is reported at the next use, with the cause taken from
the provenance list, not at the point where the invalidating operation
happened.

## 2. Core Model

### 2.1 Identity vs Name

A binding is a name. A resource is a node in the resource graph. The two are
independent: a binding is either `bound(node)` or `empty`. Moving a name does
not create, copy, or destroy a resource. Phrases such as "`b` becomes dead"
are wrong: after an alias move, `b` is `empty`, and the node it named keeps
whatever state it had.

### 2.2 Node State

Each resource node carries a content state:

| State | Meaning |
|---|---|
| `uninitialized` | The slot exists but holds no valid content. Produced by a declaration without initializer, by a physical move out of the slot, or by a completed logical move. The slot is accessible and accepts writes. |
| `taken` | The logical ownership of the address or slot has been transferred elsewhere. The slot is a husk: reading, writing, or `:=` on it is an error until the take resolves. |
| `ok` | The slot holds valid content. |

Transitions:

| From | To | Trigger |
|---|---|---|
| - | `uninitialized` | Declaration without initializer |
| `ok` | `uninitialized` | Physical move (the content relocated, the slot stays) |
| `ok` | `taken` | Logical move (logical ownership of the address transferred) |
| `taken` | `uninitialized` | Logical move completed: the holder consumed or destroyed the resource, freeing the address |
| `taken` | `ok` | The holder returned the same resource to the slot, preserving identity and provenance |
| `uninitialized` | `ok` | `=` or `:=` |

`taken` and `uninitialized` are distinct states, not spellings of one empty
state. `uninitialized` is accessible but empty. `taken` is inaccessible
because another party holds the logical ownership, and writing there would
overwrite a resource owned elsewhere.

### 2.3 Edge State

Each non-owning edge (`&`, `&mut`, `^`) is an object with its own lifecycle:

| State | Meaning |
|---|---|
| `active` | The edge is live and usable. |
| `ended` | The edge terminated normally: end of region or scope for `&`/`&mut`, explicit end for `^`. |
| `invalid(cause)` | The edge is no longer usable. The cause comes from the provenance list. |

All three edge forms are lazy: a failure is reported at the next use of the
edge, not when the invalidating operation ran. `&` and `^` differ in how the
failure reads. `&` reports that the source cannot be moved while references
are active (the mover is at fault, because `&` pins). `^` reports that it
became invalid because the resource was moved or relocated (the user of the
bind is at fault, because `^` does not pin).

An `invalid` edge no longer contributes to the flow aggregation of its node.
A binding holding an `invalid` edge keeps failing at use until retargeted
with `=`.

### 2.4 Flow

Flow is the aggregate access mode of a node, derived from the edges currently
`active` over its subgraph. It is never stored independently.

| Flow | Meaning |
|---|---|
| `neutral` | No active edges. The owner may read, write, or move. |
| `reading` | One or more active read edges. |
| `mutating` | One active writable edge. |

Flow is consulted at three places:

1. Pinning (NRA-4): a subgraph whose flow is not `neutral` cannot be
   physically relocated or consumed.
2. Boundaries (NRA-1): call, return, flow crossing, closure capture.
3. Thread frontiers (NRA-10, NRA-11).

Ordinary same-flow access is permissive. In-place mutation in the same flow
remains visible and valid through live read edges, because a reference is an
alias to the same resource, not a snapshot.

Exclusivity is enforced at the instant of each mutation (NRA-9), at the end
of each expression for local conflicts, and at thread frontiers for
cross-flow access. The "instant" is an individual load or store, or a
guarded block for `MultiWrite.sync()`.

### 2.5 Thread Separation

Threads are proven separately. Each flow is analyzed as an independent
program over its own resource graph. NRA does not track how many flows hold
a resource, and there is no `forkCount` or equivalent dynamic counter in the
core model. Composition safety comes from construction:

- A bounded flow is merged before the end of the creating scope. The compiler
  checks this structurally.
- An unbounded flow has its access revoked automatically at the end of the
  creating scope, through the revocable proxy of ADR-0026.

The previous `MultiShare` transport type is dissolved. What it modeled is now
covered by the two construction rules above.

### 2.6 Capabilities

Capabilities license what plain types cannot do:

| Capability | Meaning |
|---|---|
| `MultiWrite` | Several flows may write the same resource. Non-blocking by default: normal access is a monad-like `Ok<T> | Nil` that tries the lock and yields `Nil` on failure. `.sync()` is the compiler-generated blocking form: it waits for the lock and gives exclusive access for a scope, intended as `data.sync() \|> { ... }`. Results never carry a reference out of the locked use. |
| `SyncWrite` | Atomic access: single load/store operations without a lock. It is a capability that atomic types implement. NRA only checks that the type is atomically capable (natural size and alignment). |
| `Allocator` | Allocation provenance. Parameterized by a comptime MRA region, heap, or pool; see section 8. |
| `Transferable` | Explicit advanced transfer of allocator-owned structure across memory domains. Never implicit. |

The revocable prefix `'` or `grant` (for example `'%mut T`, the revocable
mutable own) changes nothing for NRA itself: it is a contract required at
unbounded thread boundaries, and the analysis treats the resource as an
ordinary owned resource within each flow. It is mentioned here so the reader
knows where the spelling lives; the details are thread-model material
(ADR-0026 and `docs/plans/callable-thread-blueprints.md`).

### 2.7 Provenance

Every node and edge carries a provenance list: the chain of events that
produced its current state (creation site, moves, takes, invalidating
operations). Provenance exists for diagnostics. Lazy error reporting reads
the cause from this list. Provenance is metadata, not proof state.

### 2.8 Allocation Site

Each node records the origin of its storage: stack, heap or allocator,
literal, static, or temporary, plus the guardian responsible for the final
storage free. NRA uses allocation provenance to reject `free` or `release`
from a different allocator and to decide whether a block is `ok`, `taken`,
or still pinned. MRA provides the region identity and permissions; MRA does
not prove which block is owned.

## 3. Proof Rules

Each rule has a stable id. Diagnostics reference these ids.

| Id | Rule |
|---|---|
| NRA-1 | Boundary exclusivity. At a boundary (call, return, flow crossing, closure capture), many readers or one writer may cross for a given resource, never both. `&mut` and writable binds count as writers, by declared type even when they never write. |
| NRA-2 | No dead access. Reading an `empty` binding or a node in `uninitialized` or `taken` state is an error, including through `raw`. `raw` opts out of other analyses, never of this rule. |
| NRA-3 | Region containment of `&`. A `&` cannot be stored in a field, global, or durable capture. It may be passed and returned. A returned `&` of a parameter uses the caller's region. A returned `&` of a local is rejected. An immediate aggregate that does not outlive the source may hold `&`. |
| NRA-4 | Pin of `&`. While a `&` edge is `active`, its subgraph cannot be physically relocated or consumed. The veto covers the borrowed node, its descendants, and its ancestors, not siblings: a borrow of `s.a` does not conflict with `s.b`. |
| NRA-5 | Lazy validity of `^`. A bind is valid at use if and only if its target has not been consumed or relocated since the edge was created, except when the same resource is later restored or returned to the target slot, which makes the bind valid again. Failure is reported at the next use with the cause from the provenance list. |
| NRA-6 | Acyclicity. The combined own+bind dependency graph is acyclic, checked with SCC or DFS. Object-reference cycles that add no lifetime dependency are allowed. Union-find groups nodes but is not a cycle detector. |
| NRA-7 | Completeness at boundaries. An aggregate crossing a boundary has every ownership-relevant field in `ok` state. A field projection needs only its own subgraph. An aggregate may be locally partial, for example after a field take. |
| NRA-8 | Revival by `:=`. `:=` is valid only on an `uninitialized` slot and installs a new resource identity. It never overwrites an `ok` slot and is never valid on a `taken` slot. On edges it does not exist; retargeting a reference or bind uses `=`. |
| NRA-9 | Mutation exclusivity at the instant. At the instant of a mutation, no other access to the same subgraph is in progress. In `c = a + b`, `a` and `b` are reading and `c` is mutating at that instant. Coexisting writable edges are legal; simultaneous effective access is not. Same-flow enforcement happens at the mutation instant and at expression end. |
| NRA-10 | Thread frontier. A bounded flow is merged before the end of the creating scope. An unbounded flow is revoked at the end of the creating scope. NRA proves each flow separately and checks these two construction rules at the frontier. |
| NRA-11 | Capability gating. Writing a resource reachable from another flow requires `MultiWrite` or `SyncWrite`. Without the capability, the write is rejected at the frontier. The effect header records `write`; the caller-side check sees that a flow is being created and applies this rule. |
| NRA-12 | No own of an erased outer type. `own` requires a known storage guardian (section 2.8). An `opaque` keeps only a runtime type tag, and a dynamic projection `dyn T` is a reference plus an `outerTypeId` and an `offset` into a container it does not own, so `own opaque` and `own dyn T` are rejected. `raw opaque` is a concrete slot and may be owned. `dyn Trait` is a trait object with a known layout, and its vtable carries the drop slot, so `own dyn Trait` is the boxed trait object and is allowed. See [ADR-0056](adr/0056-nra-erased-forms-and-spelling.md). |

## 4. Surface Forms

Each form has one canonical spelling. The spelling follows mutability: an
immutable form is written with its sigil, and a mutable form is written with
its keyword. The same spelling is used in type position and at the call site.
A plain `T` is the only form with neither.

| Canonical form | Accepted alias | Mutability | Meaning |
|---|---|---|---|
| `T` | (none) | - | Owned binding, lifetime follows the resource graph. |
| `&T` | `view T` | immutable | Read reference. Non-owning, region-bound, pins its source against relocation and consumption while live. |
| `^T` | (none) | immutable | Read bind. A general lifetime dependency, `lifetime(B) <= lifetime(A)`, without region limit or pin. Invalid if its target is consumed or relocated. |
| `%T` | (none) | immutable | Immutable own: logical ownership of an address or slot. Moving it marks the source slot `taken`. |
| `lend T` | `&mut T` | mutable | Write reference, same region and pinning rules as `&T`. |
| `bind T` | `^mut T` | mutable | Write bind. Same as `^T`, but counts as a writer at a boundary. |
| `own T` | `%mut T` | mutable | Mutable own. |

The three read forms `&T`, `^T`, and `%T` have no keyword, which is the
consequence of the rule: an immutable form is written with its sigil, so it
needs no keyword. `view` is a non-canonical alias of `&T`; the read reference
is `&T` in formatted source. The mutable sigil spellings `&mut T`, `^mut T`,
and `%mut T` are accepted on input and the formatter rewrites them to `lend T`,
`bind T`, and `own T`. `belong` is the old name of the write bind and is
removed; the current keyword is `bind`. `unique` is the old name of `own` and
is removed. `default` is a description of the plain `T` case, not a keyword.
See [ADR-0034](adr/0034-nra-surface-spellings.md) and
[ADR-0056](adr/0056-nra-erased-forms-and-spelling.md).

An erased form does not default to a read reference. A bare erased form is
legal only when the form can own, so a bare `dyn Trait` is `own dyn Trait` (the
boxed trait object), a bare `raw opaque` is the `void*` value, and a bare
`opaque` or `dyn T` is an error that must name its reference (`&opaque`,
`lend opaque`, `&dyn T`, `lend dyn T`). The general rule is one line: a bare
erased form defaults to `own` of the same form when the form is ownable, and is
an error when it is not. `dyn T` is a dynamic projection, a single pinning edge
that records the payload reference plus the `outerTypeId` and `offset` of its
container; `own dyn T` is rejected by NRA-12. See
[ADR-0056](adr/0056-nra-erased-forms-and-spelling.md).

The revocable prefix is the sigil `'` or the long spelling `grant`. It
combines with every form except a bare `T`, so `'&T`, `'^T`, `'%T`, `'lend T`,
`'bind T`, and `'own T` all exist. `'own T` equals `grant own T`. The mutable
sigil spellings `'&mut T`, `'^mut T`, and `'%mut T` are accepted on input and
the formatter rewrites them. `'T` does not exist.

The prefix appears only where the argument or type is declared. A call site
does not repeat it, because the `unbounded` launch site already marks where
revocation happens. A revocable resource provides a scoped access guard through
`.hold()`, as recorded in
[ADR-0041](adr/0041-revocable-guard-hold-spelling.md).

The `'` character is ambiguous with a character literal. Lexically, an
opening `'` followed by one character or one escape and a closing `'` is a
character literal; any other body starts a revocable qualifier. This is the
same rule Rust uses to separate `'a` from `'a'`, and it requires the scanner
to keep character literals to exactly one character or one escape.

`&` and `^` are the two spellings of the single non-owning edge of the
kernel: `&` adds a region limit and a pinning veto, `^` is the bare edge.
`mut` marks the write variant of either. Neither form creates a raw pointer,
and neither converts to `*`. Only `T` and the own forms create pointers. At a
C boundary the signature uses the correct relation instead of a pointer, and
the `c/` contract maps C functions to NRA effects.

In a type, `&T` and `&mut T` are reference forms. A prefix `&` applied to an
expression is the separate Zith-- address-of form and is not part of this
model.

Coercions. `^ -> &` and `^mut -> &mut` are free. `& -> ^` and `&mut -> ^mut`
are allowed only when the source covers the destination region, never from a
parameter. A read form never coerces to a write form, and a write form never
coerces to a read form.

## 5. Moves and Assignment

| Operation | Meaning |
|---|---|
| Alias move, `let b = a` with `a: T` or `a: %T` | `b` names the same resource identity. The binding `a` becomes `empty`. The resource is untouched. |
| Retarget, `let b = a` with `a: &T` or `a: ^T` | The non-owning edge is copied. `a` stays `bound` and usable. |
| Physical move | Content moves between struct fields or compatible value slots. The source slot becomes `uninitialized` and accepts `=`. |
| Logical move | Ownership of an address or slot transfers (`T -> %mut T`, `%mut T -> %mut T`). The source slot becomes `taken`. |
| `=` | Writes or initializes a valid or `uninitialized` slot. Also retargets edges. |
| `:=` | Revives an `uninitialized` slot by installing a new resource identity (NRA-8). Reserved for `T` and `%T`. |

## 6. Effect Headers

Every function has a derived effect header. The compiler derives it from the
function body, validates it, and serializes it so cross-module calls do not
reanalyze bodies.

For each argument the header records an effect and an origin:

| Effect | Meaning |
|---|---|
| `read` | The callee reads the argument value. |
| `write` | The callee writes through the argument. Whether the write crosses a thread frontier is decided at the call site (NRA-11), not in the header. |
| `move` | The callee consumes or transfers the argument. |
| `borrow` | The callee obtains a temporary `&` or `&mut` reference. |

Origins cover: root local, field path, temporary, literal, call result, and
heap or allocator provenance. Origin is derived compiler metadata, not user
syntax.

Return provenance is one of `returnsArgument(i)`, `returnsNewOwned`,
`returnsReference(i)`, `returnsBind(i)`. Calls whose return can branch
between several provenances use the conservative union.

Internal facts (`capture`, `escape`, `alloc`, `free`, `fork`, `merge`) are
not part of the public header. For `extern fn` and other interop boundaries
the header is provided through the `c/` contract vocabulary (ADR-0014), not a
new attribute: `read`, `write`, `borrow`, `move`, and `retain`, plus return
provenance. An effect alone is enough when the boundary is simple. When the
boundary also needs to say where a value came from or went, the contract adds
provenance (`returnsArgument(i)`, `returnsNewOwned`, `returnsReference(i)`,
`returnsBind(i)`, and argument origins). The exact `c/` surface for effects
and provenance is fixed with the interop design.

`let y = f(x)` where the header says `returnsArgument(i)` may be represented
in the resource graph as `y == x` after validation. This is an optimization
license granted by the proof, not a relaxation of ownership rules.

## 7. Cross-Flow and Threads

Stub. The accepted baseline is ADR-0026 (aggregate nodes, revocable flows)
with the spelling fixed by ADR-0034, ADR-0015 (thread fork and merge) as
amended by ADR-0035 (no `forkCount`, no `MultiShare`), and the thread frontier
rules NRA-10 and NRA-11. The current direction for job blueprints, launch
arguments, and monitoring handles is `docs/plans/callable-thread-blueprints.md`.

NRA's obligations here are exactly NRA-10 and NRA-11: prove each flow
separately, enforce merge or revoke at scope end, and gate cross-flow writes
on capabilities.

## 8. Cleanup and Allocation

Cleanup follows this order:

```text
fail -> defer -> drop -> storage free
```

`fail` handles the failure path. `defer` runs registered scope cleanup.
`drop` destroys the logical contents of the value. Storage free is the final
step and is always the responsibility of the guardian recorded at the
allocation site.

`drop` and `defer` can postpone storage free, but they do not change resource
identity. Storage is freed after NRA proves destruction and the allocation
origin is still valid.

`Allocator` is a capability parameterized by a comptime MRA region, heap, or
pool:

```text
capability Allocator(R):
    fn alloc(self, size: u64, align: u64): Ptr<R>!
    fn free(self, mem: Ptr<R>, size: u64, align: u64): unit!
    fn realloc(self, old: Ptr<R>, old_size: u64, old_align: u64,
               new_size: u64, new_align: u64): Ptr<R>!
```

`Ptr<R>` carries allocation provenance. Dynamic heaps are allowed:
`heap OsHeap` has `size: dynamic`, and the useful bounds live on
`Block<OsHeap> { ptr: Ptr<OsHeap>, len: u64 }`. NRA still verifies ownership
and lifetime for those blocks without MRA pretending the heap has a static
total size.

## 9. Diagnostics

Stub. The diagnostic catalog, with accepted and rejected examples per rule,
is a follow-up document. Each diagnostic uses the existing `E4001+` range and
references its proof rule id from section 3.

## 10. Open Areas

- The exact `c/` contract surface for `extern fn` effects and provenance.
- Custom allocator details, region parameterization, and storage semantics.
- The diagnostic catalog with accepted and rejected examples.
- Precise interaction of `fail`, `defer`, `drop`, and storage free for every
  control-flow shape.
- Lifetime rules for `^` fields on parent replacement.
- Container method families: stable (never reallocates, keeps binds valid)
  vs reallocating.

## 11. Related Documents

- `docs/proof-model.md` for the ZPK umbrella and the NIA, RRA, MRA siblings.
- `docs/nia-spec.md` for numeric interval facts and headers.
- `docs/rra-spec.md` for region relationships and geometric bridging.
- `docs/mra-spec.md` for static memory regions and raw access intrinsics.
- `docs/adr/0033-nra-reference-model-and-bind.md` for the accepted reference
  model.
- `docs/adr/0034-nra-surface-spellings.md` for the sigil spellings and the
  legacy keyword equivalences.
- `docs/adr/0056-nra-erased-forms-and-spelling.md` for NRA-12 (no own of an
  erased outer type) and the sigil/keyword mutability rule.
- `docs/adr/0057-bare-union-members-and-const-values.md` for the bare rule on
  union members and const values.
- `docs/adr/0026-nra-aggregate-nodes-and-revokable-flows.md` for aggregate
  nodes and revocable flows.
- `docs/adr/0035-nra-thread-separation-supersedes-forkcount.md` for the
  per-flow thread proof that removes `forkCount` and `MultiShare`.
- `docs/impl-status.md` for what Zith-- implements today (SRA).
