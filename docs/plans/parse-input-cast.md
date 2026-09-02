# Step — ParseInput and InputLine.cast<T>

## Goal

After this step, `InputLine` exposes `cast<T>` backed by a `ParseInput` trait with
`fn parse(self: view InputLine): ?Self`. Zith-- can parse primitive values from
line input through `line.cast<i32>()`, `line.cast<bool>()`, `line.cast<f32>()`,
`line.cast<f64>()` and `line.cast<u32>()`. `i32` is also added to the
`Formatable` stdlib surface so `print`/`println` can render signed
32-bit values.

The step does not implement parsing for `*char`. Strings from `InputLine` remain
available through the existing `text()` adapter. A user-defined type is allowed
to implement `ParseInput` in later work, but this step only ships and tests the
primitive implementations.

## Prerequisites

- Generic trait bounds already work for `T: Trait` call sites
  (`E3009` enforced by `checkGenericConstraints`).
- Trait conformance already registers and checks nominal traits
  (`ConformanceTable::satisfies`).
- `?Self` must type-check in trait signatures and after
  `substituteSelf` for the implementing owner.
- Generic methods already lower through the instantiation pass. The missing
  piece is the `?Self` return inside `ParseInput` and the stdlib glue.

## Baseline Facts

- `InputLine` in
  [console.zith](/home/diogo/Zith/stdlib/std/io/console.zith:7) exposes
  `text`, `len`, `good` and `destroy`; it does not expose `cast<T>`.
- `Formatable` and its primitive implementations live in
  [format.zith](/home/diogo/Zith/stdlib/std/io/format.zith:5).
  It currently implements `[]char`, `bool`, `f32`, `f64`, `u32` and `*char`;
  `i32` is missing.
- `T.method()` on a generic parameter already resolves through declared trait
  bounds in
  [sema-method.cpp](/home/diogo/Zith/src/sema/sema-method.cpp:155).
  It uses `substituteSelf` so a trait signature with `Self` becomes usable for
  the concrete generic argument.
- `InputLine.cast<T>` needs a generic method whose type argument `T` is
  constrained by `T: ParseInput` and whose return type is `?T`.

## Design Contract

1. Define `ParseInput` in `std/io/format` next to `Formatable`:

   ```zith
   pub trait ParseInput {
       fn parse(self: view InputLine): ?Self;
   }
   ```

2. Implement `ParseInput` in `std/io/format` for `i32`, `bool`, `f32`, `f64` and
   `u32`. `*char` is intentionally not implemented by this step.
3. Add `i32` as a `Formatable` primitive with signed numeric formatting.
4. Add `cast` to `InputLine` in `std/io/console`:

   ```zith
   fn cast<T: ParseInput>(self: view InputLine): ?T {
       return T.parse(self);
   }
   ```

5. `cast<T>` is a view method and does not consume `InputLine`; the caller still
   owns the line and must call `destroy` when finished.
6. Failed parsing returns `null` through the optional return. There is no extra
   error diagnostic, abort, or checked cast variant in this step.
7. `T.parse(self)` resolves through the `ParseInput` bound, not through a
   built-in per-primitive call path.
8. `i32` formatting uses signed notation. `u32` keeps the existing unsigned
   path.

## Implementation Steps

### Frontend/sema changes

1. Confirm trait signatures with `?Self` return type resolve correctly when the
   owning type is a primitive. If primitive conformance does not already lower
   `Self` on return types, fix the conformance/substitution path generically;
   do not special-case `ParseInput`.
2. Confirm a generic method with `T: ParseInput` and no other usable type
   occurrence can infer/check `T` when called as `line.cast<i32>()`. Explicit
   generic method arguments are the canonical call form in this step.
3. Confirm `T.parse(self)` on a generic parameter resolves through the bound and
   substitutes `Self = T`. Reuse the existing bound-aware method path rather
   than adding a static-method lookup for each primitive.
4. If method-call resolution returns a generic trait requirement in a generic
   method body, use the instantiated method signature so `?Self` becomes `?T`.

### stdlib changes

1. In `format.zith`, add:

   ```zith
   pub trait ParseInput {
       fn parse(self: view InputLine): ?Self;
   }
   ```

2. Add parse helpers for `i32`, `bool`, `f32`, `f64` and `u32`, implemented in
   terms of C library functions already available to the module. Follow the
   existing `format*` helper pattern and avoid adding raw debug prints.
3. Add `implement i32 as ParseInput`, `implement bool as ParseInput`,
   `implement f32 as ParseInput`, `implement f64 as ParseInput` and
   `implement u32 as ParseInput`.
4. Add `implement i32 as Formatable` and a `formatI32` helper with signed
   formatting.
5. In `console.zith`, add `cast<T: ParseInput>` to `InputLine`.

### Cache/import concerns

1. `ParseInput` lives in `format` and is visible to `console` through the
   existing `from std/io/format` import.
2. `Primitive implements` for `ParseInput` must be persisted by the same
   mechanism used for `Formatable` so cached artifacts agree with cold builds.
3. Generic method bounds must be serialized if `InputLine.cast` participates in
   cached artifacts.

## Diagnostics

No new diagnostic code is planned. Existing generic errors (`E3001`,
`E3009`, `E3010`, `E3011`) must not regress, and `line.cast<T>()` with a type
that does not satisfy `ParseInput` should report `E3009`.

## Verification

Add or extend `tests/test-parse-input.cpp` and register it with `add_zith_test`.

Acceptance source:

```zith
from std/io/console

fn parseDemo(line: view InputLine): i32 {
    let n = line.cast<i32>();
    if (n is null) {
        return 1;
    }
    return n?;
}

fn main(): i32 {
    return 0;
}
```

Assertions:

- `cast<i32>` returns `?i32` and does not consume the line.
- `cast<bool>`, `cast<f32>`, `cast<f64>` and `cast<u32>` type-check.
- A type without `ParseInput` passed to `cast<T>` reports `E3009`.
- `i32` can be passed to `println` and lowers as `dyn Formatable`.
- An invalid numeric parse returns `null` at runtime.
- The stdlib module builds and imports cleanly.

Commands:

```bash
cmake --build build -j --target test-parse-input
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `docs/20-standard-library.md`: document `InputLine.cast<T>` and the primitive
  `ParseInput` surface.
- `docs/impl-status.md`: move the `ParseInput` / `InputLine.cast<T>` debt to
  Working once primitives ship.
- `docs/adr/0004-stdlib-io-format-contract.md`: update the consequence paragraph
  that currently records `ParseInput` as not shipped.
- `docs/implementation-debt.md`: mark debt 8 as planned/implemented and keep
  `*char` parsing out of scope.
