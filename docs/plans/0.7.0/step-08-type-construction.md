# Step 08 — Comptime Type Construction

## Goal

After this step, `type Custom = @struct`, `@appendField`, `@removeField`, and
`@appendMethod` create finalized usable structs with frozen type state.

## Prerequisites

- step-06 (comptime interpreter).
- step-07 (metadata/intrinsic infrastructure, specifically the `@fields` pathway).

## Baseline Facts

- `type Custom = T` creates a nominal type via `isNominalType`.
- `@appendField` is a top-level “macro invocation” that is currently tolerated by
  `skipMacroInvocation`; it is in `kIntrinsicNames` but not otherwise handled.
- No mutable type-construction state exists.

## Design Contract

1. Add an `UnfinalizedType` registry owned by comptime/type intern.
2. `type Custom = @struct` creates an unfinalized nominal type with no fields.
3. `@appendField Custom, x: i32;`, `@removeField Custom, x;`, and
   `@appendMethod Custom, fn ...` apply to the unfinalized record.
4. A type is finalized on first instantiation, on being returned from a `const fn`,
   or on use as a generic argument. After finalization the registry record moves to
   the ordinary `StructType` intern table.
5. `@appendField` on a finalized type, public type alias, or builtin type reports
   `E6004` naming the finalizing site.
6. Method addition reuses `ownerName` and the step-01 member storage.
7. `@removeField` is rejected if the field does not exist, with a NoMember-style
   message but `E6004` only for finalized state.

## Implementation Steps

1. Add `UnfinalizedType` and its ops in `src/comptime/type-construction.cpp/hpp`.
2. Parse top-level and expression-position `@appendField`/`@removeField`/
   `@appendMethod` as real declarations when an unfinalized receiver exists.
3. Hook finalization into step-04 instantiation, `const fn` return lowering, and
   generic argument processing.
4. Reuse `internStruct`/`StructType` for finalized result.
5. Add cache round-trip for unfinalized state before finalization.

## Diagnostics

Add:

- `E6004 TypeAlreadyFinalized`.

## Verification

Add `tests/test-type-construction.cpp`:

```zith
type Custom = @struct;
@appendField Custom, x: i32;
@appendMethod Custom, fn get(self): i32 { return self->x; }
const fn makeCustom(): Custom {
    let c: Custom = Custom { x: 4 };
    return c;
}
fn main(): i32 {
    let c = makeCustom();
    return c.get();
}
```

Pass with return 4. Negative:

```zith
type Alias = i32;
@appendField Alias, x: i32;
```

Must report `E6004`.

Commands:

```bash
cmake --build build -j --target test-type-construction
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `impl-status.md`: type manipulation moves from Spec only to Working.
- `docs/11-comptime.md`: §11.4 becomes Working.
- `docs/roadmap.md`: F-17 complete.
