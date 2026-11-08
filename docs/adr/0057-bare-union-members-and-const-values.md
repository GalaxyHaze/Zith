# Bare Union Members and Bare Const Values

Status: accepted as a full-Zith design direction. Nothing in this decision is
implemented and Zith-- is unchanged.

A `union` member and a `const` value are both positions where an access or
lifetime qualifier is a category error. This ADR fixes one rule for both: the
position is bare, and the qualifier lives on the value that owns the position.

## Context

A `union` is a runtime-tagged value that contains one of its declared member
types ([03-type-system.md](../03-type-system.md)). A member is an alternative
shape of one value, not a named slot with its own identity. A qualifier such as
`own` or `&` names how a value relates to storage, and that relation belongs to
the union value as a whole, not to one alternative inside it.

A `const` value in full Zith is a compile-time value. The purpose of a const
value is that it can be copied directly, so it cannot hold an access or
lifetime qualifier that would make it a non-copyable edge. Zith-- is different:
its `const` is an immutable runtime variable, so this rule does not apply
there.

## Decision

### 1. Union members are bare

A `union` member carries no access or lifetime qualifier. A qualifier on the
union value applies to the whole slot.

```zith
union Incorrect { own Point, &i32, bind Node }   // error: access qualifiers on members
union Correct   { Point, i32, bind Node }

let x: mut Correct;    // the qualifier lives on the value
let y: &Correct;
let z: bind Correct;
```

The rule forbids the access qualifiers (`own`, `&`, `lend`, and the `%`/`&mut`
spellings). It allows the lifetime qualifier `bind` (`^mut`) and the read bind
`^`, because a stored back-reference is intrinsic to the member and is not an
access mode of the union slot. `&` on a member would already be rejected by
NRA-3, which forbids storing a `&` in a field, so the rule only names the
remaining cases.

The rule is specific to `union`. A `struct` field is a named position with its
own identity, so it may still carry a qualifier, as in the `?own Self` and
`?belong Self` of a list node. An `enum` has named constants of one underlying
type, not member types, so it has no member position to qualify.

### 2. A const value is bare

A `const` value carries no qualifier at all, neither an access or lifetime
qualifier nor `mut`. The value is bare because the intent is that it can be
copied directly.

```zith
const X: view i32 = 5;      // error: const value is not bare
const Y: i32 = 5;           // ok
```

Compile-time mutation lives inside the compile-time context, not on a `const`
binding. A `var` declared inside a `const { ... }` block or the body of a
`const fn` is a normal mutable local that is evaluated at compile time. The
`const { ... }` block guarantees its contents are computed at compile time. A
`const fn` may take references and return them during evaluation; only the
value that is stored as a `const` has to be bare.

This removes the `const counter: mut` form. The replacement is a `var` inside
a compile-time context:

```zith
const total {
    var counter = 0;
    counter += 1;
    counter
}
```

### 3. An enum is a trivial compile-time value

An `enum` is a trivial, copyable value: at runtime it is a number, the index
of the variant. A `const` of an enum type is therefore bare by nature, and no
qualifier is meaningful on it. Taking a view of an enum value in runtime code
is still allowed (`&SomeEnum`), because the rule constrains what a `const`
value may carry, not what a runtime expression may reference.

## Consequences

- A `union` member is bare. The qualifier moves to the union value.
- A `const` value is bare. `const X: mut` and `const X: view T` are errors.
- Compile-time mutation is a `var` inside `const { ... }` or `const fn`.
- The `const counter: mut` example in the comptime chapter is removed.
- An `enum` stays a trivial compile-time value and needs no member rule.

## Effect on Zith--

Zith-- is unchanged. Its `const` is an immutable runtime variable, not a
compile-time value, so the bare rule does not apply. Its `union` surface is
unchanged and this ADR does not add or remove a Zith-- member rule.
