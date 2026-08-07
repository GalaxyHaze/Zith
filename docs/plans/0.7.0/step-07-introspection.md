# Step 07 — Static Introspection

## Goal

After this step, `@fields`, `@hasTrait`, kind predicates, and comptime-unrolled
`for (f in @fields T)` are implemented.

## Prerequisites

- step-03 (`ConformanceTable::satisfies` for `@hasTrait`).
- step-04 (generic-/type-solved interned types for predicates).
- step-06 (comptime evaluator evaluates the intrinsic results).

## Baseline Facts

- `kIntrinsicNames` already contains `"fields"`, `"hasTrait"`, `"struct"`,
  `"component"`, `"union"`, `"enum"`, `"nullable"`, `"primitive"`, and more.
- `for (x in xs)` is a dedicated parse error: `for iterator form is not implemented
  yet`.
- Only `offsetOf`/`alignOf`/`sizeOf` currently produce a formal `LayoutIntrinsic`
  expression.

## Design Contract

1. Add a comptime field-metadata record type with `name`, `type`, `visibility`,
   and `offset`.
2. `@fields T` evaluates to a comptime field list; runtime use outside comptime
   reports `E6001`.
3. `@hasTrait T, Trait` uses `ConformanceTable::satisfies`.
4. Kind predicates evaluate with `TypeKind` inspection:
   - `@struct`, `@component`, `@union`, `@enum`, `@primitive`, `@nullable`.
5. `for (f in @fields T)` is desugared to a comptime unroll, one instance per
   field, with `f` bound to metadata.
6. Layout intrinsics return constants in comptime context as in step-06.

## Implementation Steps

1. Extend `ExprKind` with `IntrospectionIntrinsic` carrying `Which`.
2. Recognize the full intrinsic list in `parsePrimary`.
3. Implement sema typing for the intrinsic result values.
4. Implement interpreter evaluation for `@fields`, `@hasTrait`, and predicates.
5. Implement `for` iterator desugaring for the `@fields` form only; runtime
   iterators are deferred to the capability step.
6. Add formatter and cache serialization for `IntrospectionIntrinsic`.

## Diagnostics

No new codes. Use `E6001` for runtime use and existing type errors for malformed
arguments.

## Verification

Add `tests/test-introspection.cpp`:

```zith
struct S { x: i32, y: f32 }
const fieldsCount = @len(@fields S);
const hasPoint = @hasTrait S, Printable;
const isStruct = (S is @struct);
fn main(): i32 {
    var total: i32 = 0;
    for (f in @fields S) { total += 1; }
    return total;
}
```

Assert `fieldsCount == 2`, `isStruct == true`, and runtime total `== 2`.

Commands:

```bash
cmake --build build -j --target test-introspection
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `impl-status.md`: reflection intrinsics move from Spec only to Working.
- `docs/11-comptime.md`: reflection section becomes Working.
- `docs/roadmap.md`: F-17 complete (except type construction, covered by step-08).
