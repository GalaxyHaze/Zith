# Full-Zith Safety Levels: safe, trust, hazard

Status: accepted as a full-Zith design direction. It does not change Zith--.

Full Zith replaces the `raw`/`unsafe` function kinds with three written safety
levels on callables (`safe`, `trust`, `hazard`) and two level-changing blocks
(`hazard { }`, `safe { }`). Safety becomes a static classification inferred
from the reachable body, pinned by a written level, and sealed at the call site
by `trust`. It reuses the inference model of
[ADR-0039](0039-full-zith-capability-and-purity-contracts.md) as a second,
orthogonal dimension.

## Raw operations and blast radius

A raw operation bypasses a compiler check. The two classes are separated by
blast radius, not by syntax:

| Class | Spelling | Blast radius |
|---|---|---|
| Light | `raw` | Breaks only when misused, so correct input contains the damage. C interop, `raw opaque`, `raw x` extraction, `raw a[i]` and `raw a[lo..hi]`, raw-union member access, `raw opaque as T` reinterpret. |
| Heavy | `hazard` | Breaks assumptions the compiler relies on, so it can corrupt or miscompile even when written correctly. Inline assembly, pointer arithmetic, arbitrary address assignment and `&x` escape, bit-cast, mutability bypass. |

`raw` therefore keeps one meaning in full Zith: the light operation class.
`raw opaque` and `raw union` keep their type-modifier sense, because that is a
type property rather than an operation. `raw macro` is a Zith-- expansion
property and has no full-Zith counterpart, since full Zith has no macros.
Zith-- keeps `raw fn` and `unsafe` unchanged, so the two senses never meet in
one surface.

The three level spellings and the two block spellings stay within the
eight-byte keyword limit of
[ADR-0040](0040-full-zith-surface-names-and-keyword-limit.md).

## Safety levels

Two ladders are in play. The permission ladder is what a body may use. The
effect ladder is what a caller sees.

| Body | Permission | May open `hazard { }` | Effect at the call site |
|---|---|---|---|
| `safe fn` | safe | no | safe |
| unannotated | raw | no | inferred: safe, raw, or hazard |
| `trust fn` | raw | yes | safe (sealed) |
| `hazard fn` | hazard | yes, redundant | hazard |

An unannotated callable is classified from its reachable body and is not
presumed unsafe. A body that reaches a light raw operation is dirty and
propagates the raw effect. A body that reaches a heavy raw operation or calls a
hazard callable propagates the hazard effect. A body that reaches nothing raw
is safe. Inference is transitive through calls and reaches a least fixed point
for recursive groups, exactly as ADR-0039 defines for purity. Cleanup actions
registered in the body (`defer`, `drop`) participate in the classification.

`trust` is the only level that seals: its body may reach raw and hazard work,
and the call site still sees safe. It is an assertion the compiler cannot
prove, so it is the audited boundary of a module. The other three levels
propagate their effect unchanged.

## Blocks

The two blocks are regional casts of the permission ladder. `hazard { }` raises
the region to hazard permission and is the only place a `trust fn` may use a
heavy raw operation or call a hazard callable. `safe { }` revokes every raw
operation and hazard call in the region, so it is a compiler-verified raw-free
region rather than an asserted one. Neither block changes the effect at the
call site. The seal is always the callable level.

`safe { }` is legal in `trust fn` and in `hazard fn`. It is redundant in a
`safe fn` and draws a warning. `hazard { }` is an error in a `safe fn`, and is
redundant in a `hazard fn`, where it also draws a warning. A cleanup action
registered inside a `safe { }` that reaches a raw operation is an error,
because the region would no longer be raw-free.

There is no `trust { }` block. `trust` names a seal, and a block has no call
site to seal, so the concept has a callable form and no block form.

## Declarations without a body

An `extern fn` and any other declaration that supplies a contract instead of a
body is hazard by default, because calling unverified code propagates the
hazard effect. A written level overrides it. `trust extern fn` is the audited C
binding, and a `safe extern fn` that reaches foreign code draws a warning.
This follows ADR-0039: extern and raw declarations supply trusted contracts
rather than body proofs.

## Callable types and closures

A callable type carries its level, such as `safe fn(i32): i32` or
`hazard fn(): void`. A closure is classified from its body like any callable,
and its inferred type carries the level. A bare `fn(...)` at a boundary is the
safe level, so passing a hazard callable where `fn(...)` is expected is an
error. The exact compatibility ordering between the three callable types
remains open, mirroring the open question ADR-0039 records for purity.

## Relationship to purity and to Zith--

Safety and purity are orthogonal dimensions. A body that reaches a raw
operation and is marked `@pure` is an error, reported by the purity
contradiction diagnostic, because the violated obligation is the purity
assertion. `@pure` and `trust` are both assertions over a body and share the
same treatment: a pin that is diagnosed on violation, a warning when the
assertion is unnecessary, and per-module accounting for audit.

The safety level enters the public ABI hash and the cache serialization, so a
level change on a `pub fn` invalidates its callers. This matches how
`discardable` already travels.

A dedicated compile-time diagnostic family reports safety-level violations,
including the unnecessary `trust` warning, the redundant block warnings, the
`safe` pin violation, and the raw cleanup inside a safe block. Its numeric code
range is not assigned here, as with the purity family in ADR-0039.

Zith-- keeps `raw fn`, `unsafe`, and its `#[...]` attributes unchanged. Zith
uses different function kinds and abandons `raw fn` and `unsafe`. This is one
of the points where the sibling language contracts diverge.

The `trust fn` level replaces the `Trust` capability, which full Zith removes.
The capability was nominal and per-type, exposing `raw fn` methods through a
trait. The level is structural and per-callable, so it covers the same case
without a trait and limits the vouched surface to the callables that carry it.
No narrower role for the capability is retained.
