# Step 05 — Generic Constraint Enforcement

## Goal

After this step, generic bounds like `T: Printable + Clone` are enforced at call
time, method uses on generic parameters are checked against declared bounds, and
errors are reported once at the declaration when no bound exists.

## Prerequisites

- step-03 (`ConformanceTable::satisfies`).
- step-04 (instantiated generic bodies must exist to place bounds).

## Baseline Facts

- `GenericParam::constraint` is parsed but not used by sema.
- `PerModuleSema::genericParams_` stores name and type but not constraint IDs.
- Inside a generic body, `T` may already resolve to `GenericParam`; no bound-aware
  method resolution exists.

## Design Contract

1. Extend `GenericBinding` with the parsed constraint refs from
   `frontend::GenericParam::constraint`.
2. A bound is satisfied with `ConformanceTable::satisfies(arg, bound)`.
3. Multiple bounds use `+`; the frontend parser must produce one `ConstraintRef`
   per bound. This is implemented here, not in step-02.
4. At a call to a constrained generic:
   - resolve concrete type args,
   - check every bound,
   - on failure report `E3009` on the argument expression, naming the bound, the
     offending argument type, and the call site.
5. Inside a generic body, method call resolution on `T` is limited to methods on
   the declared bounds. A method with no matching bound reports `E3001` at the
   declaration, once, with help “add T: X to the generic parameter list”.
6. Implicit constraints from usage are explicitly out of scope.

## Implementation Steps

1. Parse `A + B` bounds into `GenericParam::constraint` as a list (frontend).
2. Thread the bounds through `PerModuleSema`'s generic bindings.
3. Call `ConformanceTable::satisfies` during instantiation in the step-04 pass.
4. Add bound-aware method lookup for `GenericParam` targets.
5. Emit single-error diagnostics without speculative cascade from later members.

## Diagnostics

Add only:

- `E3009 ConstraintNotSatisfied`.

## Verification

Add `tests/test-generic-constraints.cpp`:

```zith
trait Printable { fn print(self); }
fn log<T: Printable>(v: T): i32 { v.print(); return 0; }
struct Point { x: i32 }
implement Point as Printable { fn print(self) { } }
fn main(): i32 { let p: Point = Point { x: 1 }; return log(p); }
```

Pass. Negative:

```zith
struct NotPrintable { x: i32 }
fn main(): i32 { let p = NotPrintable { x: 1 }; return log(p); }
```

Must report exactly `E3009` naming `Printable`.

Commands:

```bash
cmake --build build -j --target test-generic-constraints
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `impl-status.md`: generic constraints move to Working (explicit bounds only).
- `docs/roadmap.md`: F-29 complete.
- `docs/03-type-system.md`: note implicit inference remains unimplemented.
