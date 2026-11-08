# NRA Erased Forms, Explicit Reference Qualifiers, and Sigil/Keyword Spelling

Status: accepted as a full-Zith design direction. Nothing in this decision is
implemented and Zith-- is unchanged.

This ADR settles three NRA surface questions left open by
[ADR-0033](0033-nra-reference-model-and-bind.md) and
[ADR-0034](0034-nra-surface-spellings.md): whether an erased form can carry
`own`, whether a bare erased form may default to a reference, and when the
sigil spelling and when the keyword spelling is used. It adds proof rule
NRA-12, supersedes the "the choice is stylistic" sentence of ADR-0034, and
removes the implicit read reference from `opaque` and `dyn`.

## Context

### Erased forms

`opaque` and `dyn T` are erased forms. An `opaque` value keeps a runtime type
tag and erases its outer type, so it is a tagged open union stored as
`{ *void, u32 }` ([03-type-system.md](../03-type-system.md)). A `dyn T` value
is a dynamic projection: the inner type `T` is known and the outer type that
supplies the storage is erased. `dyn Trait` is different again. It erases the
concrete implementation but keeps a known layout, the two-word data plus
vtable fat pointer ([ADR-0049](0049-optional-dyn-niche-representation.md)).

The NRA requires that an `own` name a storage guardian. Section 2.8 of
[nra-spec.md](../nra-spec.md) states that each node records the guardian
responsible for the final storage free, and that NRA uses allocation
provenance to decide whether a block is `ok`, `taken`, or still pinned. An
`own` of a form whose outer storage is unknown has no guardian to name.

### Implicit references

Today an erased form defaults to a read reference. `opaque` is "a reference
type (`view` by default)" ([03-type-system.md](../03-type-system.md)) and is
"always a view" ([impl-status.md](../impl-status.md)). `dyn Trait` is "a
`view` by default" ([14-polymorphism.md](../14-polymorphism.md)). The default
hides a transfer mode, so a bare `opaque` or `dyn Trait` parameter reads like
a value but is a borrow. This decision removes that default.

### Spelling

ADR-0034 fixed a sigil and, for the write and own forms, an equivalent
keyword, and declared the two interchangeable. That left no rule for which
spelling to use where.

## Decision

### 1. An erased outer type is not ownable

`own` requires a known storage guardian, so it is rejected on a form whose
outer storage is erased.

| Form | `own` allowed | Reason |
|---|---|---|
| `raw opaque` | yes | A concrete `void*` slot, a known guardian. |
| `opaque` | no | Outer type erased, no guardian to name. |
| `dyn T` | no | Outer type erased, no guardian to name. |
| `dyn Trait` | yes | Layout known, the vtable carries the guardian. |

`own opaque` is replaced by `own raw opaque` at every site that needs an owned
opaque, including the C binding of `malloc` in
[18-c-interop.md](../18-c-interop.md). `own dyn Trait` is the boxed trait
object. The trait object is ownable because the vtable always carries a drop
slot. Most Zith types do not need a `drop` because storage free happens as a
separate final step ([nra-spec.md](../nra-spec.md) section 8), but the drop
slot is emitted on every vtable for safety, so a boxed trait object always has
a guardian to call.

### 2. A bare erased form does not default to a reference

The implicit read reference is removed. A bare erased form is legal only when
it names a value the form can own.

| Bare form | Meaning | Reason |
|---|---|---|
| `dyn Trait` | `own dyn Trait` | Ownable, so bare names the owned value. |
| `opaque` | error | Not ownable, so the reference must be explicit. |
| `dyn T` | error | Not ownable, so the reference must be explicit. |
| `raw opaque` | the `void*` value | A concrete slot, like `*T`. |

`dyn Trait` bare now means the boxed trait object, so a read parameter is
`&dyn Trait` and a mutable one is `lend dyn Trait`. A bare `opaque` or `dyn T`
is a compile error and the reference has to be written: `&opaque`/`lend
opaque` and `&dyn T`/`lend dyn T`. The general rule is one line: a bare erased
form defaults to `own` of the same form when the form is ownable, and is an
error when it is not.

The erasure cast names the reference, because the bare form does not exist:
`T as &opaque`, `T as lend opaque`, `T as ^opaque`, and `T as bind opaque` are
the four erasure casts, one per reference form. There is no `T as own opaque`
and no `T as opaque`. The extraction casts keep the checked behavior, for
example `&opaque as T`, which returns the checked result. `raw opaque` is a
value and keeps its current casts, `T as raw opaque` and `raw opaque as T`.

### 3. What a dynamic projection is

The two `dyn` forms mean different things and are not conflated. `dyn Trait`
is a trait object: the implementation is erased and a vtable dispatches.
`dyn T`, where `T` is a concrete type, is a dynamic projection: the inner type
`T` is known and the container is the erased part.

A `dyn T` is not a reference to a standalone `T`. It is a reference to a `T`
that lives inside another value, plus enough information to name that
container again. The value is three fields:

| Field | Meaning |
|---|---|
| `ref` | A concrete reference to the `T` payload at its real address. |
| `outerTypeId` | The runtime type id of the value that contains the payload. |
| `offset` | The byte offset of the payload inside that container. |

Given

```zith
struct Vec2d { [x, y]: i32 }
var p = Vec2d { x: 3, y: 4 };
let d: &dyn i32 = p.x;     // a projection of the field x
```

`d` is a reference to `p.x`, with `outerTypeId = Vec2d` and
`offset = @offsetOf(Vec2d, x)`. It names the same storage as `p.x`, not a copy
of `3`.

A projection is a single edge. It pins its container the way `&` does, so the
container cannot be relocated or consumed while the projection is live. The
`outerTypeId` is the outermost container, the one the projection was taken
from, so a nested `p.a.b` records the type of `p` and an offset from the base
of `p`.

The projection can be inverted. A cast from `dyn T` back to a concrete type
checks `outerTypeId` and the `offset`, then rebuilds a reference to the
container, so a `dyn i32` taken from `p.x` casts back to `Vec2d` rather than
to a bare `i32`:

```zith
if (d is Vec2d) { ... }    // Zith has no native ?T; the checked test narrows
```

The cast is checked, like `opaque is T`. This is what separates a dynamic
projection from `opaque`: an `opaque` keeps only a type tag and has no payload
address or offset to reconstruct the container.

### 4. Spelling follows mutability

The immutable forms are spelled with the sigil. The mutable forms are spelled
with the keyword. The rule is one line and needs no position rule, because the
same spelling is used in type position and at the call site.

| Form | Mutability | Canonical spelling |
|---|---|---|
| `&T` | immutable | `&T` |
| `^T` | immutable | `^T` |
| `%T` | immutable | `%T` |
| `&mut T` | mutable | `lend T` |
| `^mut T` | mutable | `bind T` |
| `%mut T` | mutable | `own T` |

The three read forms have no keyword, which is the consequence of the rule
rather than an exception to it: an immutable form is written with its sigil,
so it needs no keyword. `view` is demoted from a co-equal spelling of `&T` to
a non-canonical alias, and `&T` is the read reference in formatted source. The
mutable sigil spellings `&mut T`, `^mut T`, and `%mut T` remain accepted on
input as aliases and the formatter rewrites them to `lend T`, `bind T`, and
`own T`. This follows the formatter rule of
[ADR-0044](0044-full-zith-annotation-order-and-purity-rename.md): any order is
accepted on input and there is one legal spelling in formatted source.

### 5. Call-site spelling

The call-site annotation of [ADR-0038](0038-call-site-reference-annotations.md)
uses the same mutability rule. The sigil names an immutable mode and the
keyword names a mutable mode.

| Argument form | Spelling | Receives |
|---|---|---|
| `x` | none | plain value move |
| `&x` | sigil | `&T` |
| `^x` | sigil | `^T` |
| `%x` | sigil | `%T` |
| `lend x` | keyword | `&mut T` |
| `bind x` | keyword | `^mut T` |
| `own x` | keyword | `%mut T` |

`%x` maps to the immutable own `%T` and keeps the sigil. `own` maps to the
mutable own `%mut T` and is the keyword. This matches ADR-0034, where `%T` has
no keyword and `own` is the keyword of `%mut T`.

## Consequences

- NRA gains proof rule NRA-12: an erased outer type is not ownable.
- `dyn T` is recorded as a dynamic projection: a reference to the payload plus
  the `outerTypeId` and `offset` of its container. It is a single edge that
  pins the container. A checked cast back to the container type is possible,
  unlike `opaque`.
- A bare `opaque` and a bare `dyn T` are errors. The reference is explicit.
- The erasure cast names the reference: `T as &opaque`, `T as lend opaque`,
  `T as ^opaque`, and `T as bind opaque`. There is no `T as opaque`.
- A bare `dyn Trait` is `own dyn Trait`, the boxed trait object. A read
  parameter becomes `&dyn Trait`.
- `own opaque` is removed. Existing uses become `own raw opaque`.
- `view` is demoted. The read reference is `&T` in formatted source.
- ADR-0034's "the choice is stylistic" sentence is superseded. The choice is
  now the mutability rule above.
- The formatter owns the normalization, so the surface has one legal spelling
  per form in formatted source.

## Considered Options

Keeping the implicit read reference on the erased forms was rejected. The
default hides a transfer mode, so a bare `opaque` or `dyn Trait` parameter
reads like a value but is a borrow. The whole point of the change is to remove
that surprise.

A positional split, keywords in declarations and sigils at call sites, was
rejected for the spelling. It reads the same form differently by position, so
a reader has to learn the position as well as the form. The mutability rule
uses one dimension in every position.

Making `own dyn Trait` illegal was rejected. The fat pointer has a known
layout and the vtable carries the drop, so the boxed trait object has a
guardian and is a legitimate owned value.

## Effect on Zith--

This decision is full-Zith only and Zith-- is unchanged. Zith-- keeps its
current behavior: a bare `opaque` is an implicit read reference, `T as opaque`
and `opaque as T` keep their current spellings, and the compiler does not
implement the explicit-reference rule. The divergence is the expected one
between sibling languages, not an inconsistency to fix.
