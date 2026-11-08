# NRA Future Design

Short-lived operational pointer for the future Zith NRA contract. The full spec
is `docs/nra-spec.md`; the current `Zith--` implementation status is in
`docs/impl-status.md` and must not be mixed with this design.

## Decisions

### Rewrite and core model (decided 2026-10-06, grilling session)

- `docs/nra-spec.md` was rewritten from scratch. The earlier draft mixed the
  legacy `lend`/`view`/`share`/`belong` surface with the ADR-0033 reference
  model and is gone. The new spec is the only normative NRA document.
- Zith-- does not implement NRA. It implements SRA (Small Resource Analysis),
  a deliberately small frozen slice: `lend`/`view` call annotations, local
  borrow conflicts (E4002/E4003), `&local`/`@ptrOf(local)` use-after-move, and
  residual facts as HIR side tables. Contracts (`@assume`/`@ensure`/`@maybe`),
  NRA, and safety belong to full Zith. `docs/Zith---implementation.md` has an
  SRA section; `docs/07-memory-model.md` carries a legacy banner.
- The core model separates three things. A binding is `bound(node)` or
  `empty` (a name, never a resource state). A node has content state
  `uninitialized | taken | ok`. A non-owning edge (`&`, `&mut`, `^`) has edge
  state `active | ended | invalid(cause)`. `flow` (`neutral | reading |
  mutating`) is derived from active edges, never stored.
- `taken` and `uninitialized` are distinct states. `taken` means logical
  ownership of the address was transferred and the slot is a husk until the
  take resolves (`taken -> ok` on return of the same resource, `taken ->
  uninitialized` when the holder consumes it). `uninitialized` is accessible
  and accepts `=`/`:=`.
- All edge forms are lazy. `&` and `^` differ in failure wording and in
  region/pin, not in eagerness. Retargeting an invalid edge uses `=`; `:=` is
  reserved for `T`/`%T` slots.
- Mutation exclusivity is enforced at the instant of the mutation, at
  expression end for local conflicts, and at thread frontiers. Coexisting
  writable edges are legal; simultaneous effective access is not. In
  `c = a + b`, `a` and `b` read and `c` mutates at that instant.
- Threads are proven separately. There is no `forkCount` and no `MultiShare`
  in the core model. A bounded flow is merged before the creating scope ends;
  an unbounded flow is revoked at scope end (ADR-0026 proxy). The revocable
  prefix `'`/`grant` changes nothing for NRA; it is a contract required at
  unbounded boundaries.
- Capabilities: `MultiWrite` (non-blocking monad `Ok<T> | Nil`, optional
  blocking `.sync() |> { ... }` guard) and `SyncWrite` (atomics). The effect
  header records plain `write`; the caller-side check sees a flow being
  created and applies capability gating (NRA-11).
- Proof rules are enumerated with stable ids NRA-1..NRA-11 in spec section 3.

### Open areas resolved (decided 2026-10-07)

- Thread separation supersedes `forkCount`. ADR-0035 removes the `forkCount`
  counter and the `MultiShare<T>` transport; ADR-0015 and
  `docs/plans/branch-protocol.md` keep `fork`/`merge`/`revoke` and their
  handle lifecycle but their `forkCount` sections are superseded. The proof
  is per flow with merge/revoke at scope end (NRA-10).
- NRA-5 has one exception: a `^` invalidated by consumption or relocation
  becomes valid again if the same resource is restored or returned to the
  target slot. Otherwise the bind stays invalid until retargeted with `=`.
- `extern fn` effect headers use the `c/` contract vocabulary (ADR-0014),
  not a new attribute: `read`/`write`/`borrow`/`move`/`retain` plus return
  provenance when the boundary needs it.
- Diagnostics use the existing `E4001+` range and reference the NRA rule id.
  The catalog itself is a future document; section 9 stays a stub.
- Cleanup: one scope-cleanup mechanism for `defer` and `drop`, matching the
  Zith-- `defer` implementation (reverse registration, per lexical block,
  runs on every exit). OPEN: the order of `drop` vs `defer` within a scope is
  not yet decided. The spec section 8 writes
  `fail -> defer -> drop -> storage free` (defer before drop), which is the
  current default, but the alternative is drop first (innermost) then defer.

### Reference model (decided 2026-10-06, supersedes view/lend/share entries below)

- Surface forms fixed by ADR-0033, spellings fixed by ADR-0034: `&T` read
  reference (`view`), `&mut T` write reference (`lend`), `^T` read bind,
  `^mut T` write bind (`bind`, formerly `belong`), `%T` immutable own, and
  `%mut T` mutable own (`own`, formerly `unique`). The `'`/`grant` prefix
  combines with every form except a bare `T`. The old `lend`, `view`, and
  `share` categories are absorbed into `&`/`&mut`.
- Erased forms and spelling settled 2026-11-08 by ADR-0056. `own` requires a
  known storage guardian, so an erased outer type is not ownable: `own opaque`
  and `own dyn T` (dynamic projection) are rejected, `own raw opaque` is the
  owned opaque, and `own dyn Trait` stays as the boxed trait object because the
  vtable carries the drop slot. This adds proof rule NRA-12.
- `dyn T` is a dynamic projection, not a trait object: a reference to the
  payload plus the `outerTypeId` of its container and the byte `offset` of the
  payload in it. `let d: dyn i32 = p.x` on `struct Vec2d { [x, y]: i32 }` gives
  `outerTypeId = Vec2d` and `offset = @offsetOf(Vec2d, x)`. A checked cast
  `d as Vec2d` compares the id and rewinds the offset, so it rebuilds the
  container rather than a bare `i32`. `dyn Trait` is the separate vtable form.
  The projection is a single edge that pins its container like `&`, and the
  `outerTypeId` is the outermost container (the one the projection was taken
  from), so a nested `p.a.b` records the type of `p`.
- Erased forms no longer default to a read reference (ADR-0056). A bare erased
  form defaults to `own` of the same form when the form is ownable and is an
  error when it is not: a bare `dyn Trait` is `own dyn Trait` (boxed trait
  object), a bare `raw opaque` is the `void*` value, and a bare `opaque` or
  `dyn T` is an error that must name its reference (`&opaque`/`lend opaque`,
  `&dyn T`/`lend dyn T`). The legal qualifiers on `opaque`/`dyn T` are
  `&`/`^`/`lend`/`bind`; `own`/`%` are rejected by NRA-12.
- Zith-only, Zith-- unchanged: Zith-- keeps the bare `opaque` as an implicit
  read reference and keeps `T as opaque` / `opaque as T`.
- The erasure cast names the reference: `T as &opaque`, `T as lend opaque`,
  `T as ^opaque`, `T as bind opaque` (one per reference form). There is no
  `T as opaque` and no `T as own opaque`. `raw opaque` keeps `T as raw opaque`
  and `raw opaque as T`.
- Union members and `const` values are bare (ADR-0057). A union member carries
  no access qualifier (`own`/`&`/`lend` rejected; `bind`/`^` allowed for a
  stored back-reference), and the qualifier lives on the union value
  (`let x: mut Correct`, `let y: &Correct`). A `const` value carries no
  qualifier at all, including no `mut`; compile-time mutation is a `var` inside
  `const { ... }` or a `const fn` body. An `enum` is a trivial compile-time
  value (a runtime number), so a const of an enum type is bare by nature, but
  `&SomeEnum` in runtime code stays legal.
- ADR-0056 also replaces ADR-0034's "the choice is stylistic" sentence. The
  canonical spelling now follows mutability: the immutable forms keep the
  sigil (`&T`, `^T`, `%T`) and the mutable forms use the keyword (`lend T`,
  `bind T`, `own T`). `view` is demoted to a non-canonical alias of `&T`, and
  the mutable sigils (`&mut T`, `^mut T`, `%mut T`) are input aliases the
  formatter rewrites. The same rule governs the call-site annotations of
  ADR-0038 (`&x`/`^x`/`%x` vs `lend x`/`bind x`/`own x`).
- References may coexist in one flow. Exclusivity is checked only at a
  boundary (call, return, flow crossing, closure capture): many readers or one
  writer. A closure capture is sugar for passing the captured resources in an
  explicit struct, so it follows the same rule.
- Two non-owning surfaces share one edge in the NRA: a non-owning lifetime
  dependency, validated lazily at use. `&` is that edge plus a region limit and
  a pinning veto. `^` is the edge with no region limit and no pinning. `%` is
  the owning edge. The kernel has two edges, owning and non-owning.
- `&` is region-bound: it cannot be stored in a field, global or durable
  capture, but it can be passed and returned. A returned `&` of a parameter
  takes the caller's region. A returned `&` of a local is rejected. A
  region-bound closure may capture `&`, a durable one may not. An immediate
  aggregate that does not outlive the source may hold `&`.
- `&` pins: while it is live, the source cannot be physically relocated or
  consumed. The veto is checked per subgraph. A borrow of `s.a` conflicts with
  `s.a`, its descendants and its ancestors, not with sibling `s.b`.
- `^` does not pin. It is invalidated when the parent is consumed or its
  address changes (physical relocation). One rule, one effect. `let b = a`
  retargets a name, it is not a move and does not invalidate. A bind tracks
  resource identity, not a binding name, so `data := newResource` does not
  move it.
- All invalidation is reported lazily at the next use, with the cause taken
  from the provenance list. The analysis never fails mid-way. Wording differs:
  `&` says the source cannot be moved while references are active, `^` says it
  became invalid because the resource was moved or relocated.
- `bind` is a general lifetime-subset edge, `lifetime(B) <= lifetime(A)`, not a
  structural part-of edge. It may be a parameter and a return value, with
  provenance `returnsBind(i)`. A bind assumes a stable address for its target,
  so durable structures allocate their nodes. ADR-0033 illustrates a doubly
  linked list with owning `next` and bound `prev` edges. Exact optional-field
  surface syntax remains to be aligned with `&`, `^`, and `%`.
- Writable binds count as writers in the boundary rule. Coercions: `^ -> &` is
  free, `& -> ^` only when the source covers the destination region, never
  from a parameter.
- `&` and `^` never convert to `*`. Only `T` and `%T` can create pointers. At a
  C boundary the signature uses the correct relation instead of a pointer, and
  the `c/` contract maps C functions to NRA effects.
- `c/` contract vocabulary is the NRA effect-header: `read`, `write`, `borrow`,
  `move`, `retain`, plus return provenance. A borrow materializes a temporary
  pointer for the call only. `retain` must be declared when C keeps it.
  Headers imported by libclang stay raw with no contract.
- Lifetime cycles are rejected over the combined `own` plus `bind` dependency
  graph. Object-reference cycles that add no lifetime dependency are allowed.
  Cycle detection uses SCC/DFS on the directed graph. Union-find is not a cycle
  detector for directed graphs. It stays as the grouping structure, one
  disjoint-set per independent equivalence relation, with the provenance list
  kept for error messages.
- `:=` on `own` or plain `T` is allowed only on a dead slot and installs a new
  physical slot. On refs and binds it only retargets.
- A struct may be locally partial, for example after a parent died and a bind
  field became invalid. It must be complete only when it crosses a boundary. A
  field projection needs only its own subgraph.

### MultiWrite

- `MultiWrite` is a capability that lets several flows (threads) write the
  same resource. Each boundary stays exclusive, only flows may coexist. The
  type implements `acquire()`, `lock()`, `release()`. Users never call them.
- Normal access is a monad-like `Ok<T> | Nil`. It tries the lock, holds it
  only during the use, and yields `Nil` on failure. Results cannot carry a
  reference out of the locked use.
- `.sync()` is the public API, generated by the compiler. It waits for the
  lock and gives exclusive access without repeated checks. The guard is not
  stored by the user. The intended form is `data.sync() |> { ... }`: the pipe
  takes the guard, runs the block and releases it, including on early exit.

### Resource graph

- The core resource graph is hierarchical. An `AggregateNode` represents a
  complete value and contains `FieldNode` children only for ownership-relevant
  fields. Plain scalar fields do not need independent nodes.
- Resource identity is independent of bindings. A binding is a name pointing to
  an `AggregateNode` or `FieldNode`; moving a name does not recreate the
  resource.
- Aggregate moves require every ownership-relevant child to be available.
  `liveFieldCount` and `requiredFieldCount` are fast checks, not replacements
  for per-field state or diagnostic provenance.
- A partial field move consumes only that field. Sibling fields stay alive, but
  a whole-aggregate move is rejected until every required field is available.
- Returned values and fields carry node-level provenance. `returnsField` can
  identify the exact source node rather than only an argument index.

### Move and assignment semantics

- `let b = a` normally retargets the binding `b` to the resource identity
  referenced by `a`; it is not automatically a physical copy.
- A physical move is used between struct fields or compatible value slots.
  The source field becomes uninitialized and can be restored with `=`.
- A logical move consumes a storage identity, address, or ownership edge.
  `T -> own T` and `own T -> own T` are logical moves. `T -> T` and
  `own T -> T` are physical moves.
- `=` writes/initializes a currently valid or physically moved slot.
  `:=` is reserved for reviving a logically moved aggregate binding by
  creating a completely new `AggregateNode`.

### Views and flows

Written before the reference model. The read reference is `&T` (canonically;
`view` is a non-canonical alias) and the write reference is `lend T`
(`&mut T` is a non-canonical alias), per ADR-0056. Where this section
disagrees with the reference model, the reference model wins.

- `view` is lazy by default. Invalidation does not fail immediately; a later
  access to the invalidated view reports the error. A view remains associated
  with its original node ID, not with a binding resolved again later.
- A view is an alias to the same resource, not a snapshot. In-place mutation
  in the same flow remains visible and valid.
- Strict view policy is optional. It rejects conflicting moves/writes while
  anchors are active instead of deferring the error until use.
- Same-flow read/write coexistence is allowed according to ordinary NRA
  permissions. A fork creates a new execution flow and activates cross-flow
  restrictions.
- An aggregate with an active anchor cannot be transferred across flows in
  strict mode. In lazy mode the transfer can invalidate the anchor; a later
  view access then fails and the invalid anchor no longer contributes to
  lifetime.
- A field can cross a flow independently only when the transfer does not
  conflict with an anchor on that field or an ancestor aggregate. An
  aggregate-level anchor covers all relevant children.

### Thread modes

- The current thread direction is a discussion draft, not an accepted
  replacement for ADR-0015 or ADR-0026.
- The current draft is
  [`docs/plans/callable-thread-blueprints.md`](../docs/plans/callable-thread-blueprints.md).
  It records `Thread.job(work)`, launch-time arguments and mode selection,
  context sugar that returns a monitoring handle, and the unresolved callable
  and capture-reuse rules.
- The revocable-access concept is a source modifier rather than a
  `Revokable<T>` type. ADR-0034 fixes the spelling as a `'` sigil or `grant`
  before any ownership qualifier except a bare `T`, so `'view T`, `'lend T`,
  `'^T`, `'^mut T`, `'%T`, and `'%mut T` all exist (`'%mut T` is the `'own T`
  of ADR-0026). A bare `'T` does not exist because a revocable `default` would
  imply a logical move of an inline value, while the contract is a reference
  whose access can be revoked. The runtime lowering remains open.

### Revokable access (ADR-0026 runtime baseline, ADR-0041 spelling)

The following proxy contract remains the accepted runtime baseline in
ADR-0026. The source surface is now the `'` / `grant` prefix on any ownership
qualifier except a bare `T`, so no `Revokable<T>` wrapper appears in source.

- The runtime proxy is a capability over an owned handle. ADR-0034 lets the
  contract attach to any qualifier except a bare `T`, so a revocable borrowed
  handle such as `'view T` or `'lend T` is exposed.
- The proxy supports a read mode with multiple active readers and a write mode
  with one exclusive active writer.
- `hold()` returns a scoped guard that inherits the original qualifier. The
  guard validates access once, permits normal method dispatch without repeated
  revocation checks, and cannot escape its scope.
- The control block is separate from the resource. It contains the atomic
  resource pointer and atomic lease/revocation state and remains observable
  after the resource is cleaned.
- The lease state uses a numeric protocol: positive reader count, an exclusive
  writer state, and a revoking state that prevents new acquisitions. The
  pointer becomes null only after active guards are released.
- `Waiter` exposes only `wait()`. The thread implementation decides whether it
  waits for completion, merge, or revocation cleanup.
- Type erasure does not remove NRA effects. A revocable `dyn Trait` is a proxy
  around the erased object, and it does not require the trait itself to
  implement revocation. A revocable proxy adds an outer optional result layer,
  so a method returning `?T` remains distinguishable from revocation.

### Transfer and allocation

- Automatic allocator migration while crossing a thread boundary is not the
  default. The recommended unbounded pattern is flow-local construction:
  capture trivial configuration, pass an allocator to the child, and construct
  the aggregate there.
- `Transferable` may exist as an explicit advanced capability, but it must not
  silently recreate allocator-owned fields in another memory domain.
- A bounded flow may rely on parent scope lifetime. An unbounded flow may not
  rely on parent-owned stack/allocator storage unless it uses the explicit
  revocable protocol.

- Function contracts remain derived and cacheable effect headers. Effects
  include `read`, `write`, `move`, and `borrow`; `capture`, `escape`, `alloc`,
  `free`, `fork`, and `merge` remain internal facts.
- `view` is an anchor, not an owner. It never destroys or promotes storage.
- Superseded: `share` is absorbed into `&mut`. Multiple writer flows are the
  explicit `MultiWrite` capability, see the MultiWrite section.
- Cleanup order remains `fail -> defer -> drop -> storage free`.
- The current implementation status remains separate in `docs/impl-status.md`;
  this file records future NRA design decisions, not shipped Zith-- behavior.

## Rejected or deferred approaches

- Do not silently migrate strings, vectors, or other allocator-owned fields
  to a destination allocator during an unbounded transfer.
- Do not treat an atomic pointer alone as a lifetime guarantee. A pointer
  load must be protected by an active lease until the access ends.
- Do not make every worker implicitly revocable. The worker contract must
  explicitly declare revocable borrowing, even though the compiler inserts the
  wrapper at `.unbounded(...)`.
- Do not make a guard escape its lexical scope or collapse revocation `Nil`
  with a method's ordinary domain-level `Nil`.

## Open Areas

- Containers may get two method families: stable (never reallocates, keeps
  binds valid) and reallocating. Not designed yet.
- Whether a `^` can be re-bound automatically after relocation, or only by an
  explicit retarget. Today it only becomes invalid.
- `async`/`await` is out of scope and expected to stay banned. `state` is the
  intended explicit alternative.
- The representation and source spelling of revocable access are settled at
  the surface: ADR-0034 fixes `'` / `grant` before any ownership qualifier
  except a bare `T`. ADR-0026 records the runtime proxy baseline; the thread
  draft still owes how revocable forms interact with launch and merge.
- `extern fn` effect-header attributes.
- Custom allocator details.
- Diagnostic catalog and examples.
- Exact cleanup ordering across every control-flow shape.
- `^mut` (bind) field lifetime rules.
