# Step 06 — Comptime Evaluation And Const Fn

## Goal

After this step, `const { }` blocks and `const fn` calls are evaluated at compile
time over a restricted value domain, with budget enforcement and clear
not-comptime errors.

## Prerequisites

- step-04 (concrete type substitution for `const fn`).

## Baseline Facts

- `const` is a binding modifier; `const fn f()` is parsed as a `const` binding
  named `fn`, not as a function kind.
- No comptime evaluator exists; runtime code paths reach codegen unchanged.
- `@sizeOf`/`@alignOf`/`@offsetOf` lower at runtime through
  `HirLowerModern::lowerLayoutIntrinsic`.

## Design Contract

1. Add `DeclKind::ConstFunction` and parse `const fn Name(...) : Return { ... }`.
2. Value domain supported by the tree-walking interpreter:
   - integers, floats, bool, char, string,
   - arrays, structs, type values,
   - field-metadata records from step-07 when available.
3. Entry points:
   - `const { ... }` block expression,
   - `const` binding initializer,
   - calls to `const fn` only from those contexts.
4. Calling a `const fn` at runtime is `E6003`.
5. A runtime-dependent value inside a comptime context is `E6001`, citing the
   value and origin span.
6. Interpreter steps and memory footprint are budget-limited; exceeding the fixed
   budget emits `E6002`.
7. A comptime `throw "msg"` aborts compilation with `E6005` and the message.
8. `@sizeOf`, `@alignOf`, `@offsetOf` return constants in comptime contexts while
   runtime lowering remains.

## Implementation Steps

1. Add `ConstFunction` to `DeclKind` and update `declarationKind`.
2. Parse `const` prefix interaction with `fn` in `lowerDeclaration`.
3. Add `src/comptime/interpreter.hpp/cpp` with a `Value` discriminated union,
   scope stack, instruction budget counter, and evaluation over `frontend::Expression`.
4. Route `const { }` and `const` initializer evaluation through the interpreter.
5. Register `const fn` instances in the step-04 instantiation pass.
6. Add constant-folding for layout intrinsics.
7. Wire diagnostics and keep parser/formatter/cache paths in sync.

## Diagnostics

Add:

- `E6001 NotComptimeEvaluable`.
- `E6002 ComptimeBudgetExceeded`.
- `E6003 ConstFnCalledAtRuntime`.
- `E6005 ComptimeThrow`.
- `E6006 UnknownIntrinsic` for unrecognized `@` names (reserved for user clarity).

## Verification

Add `tests/test-comptime-eval.cpp`:

```zith
const fn add(a: i32, b: i32): i32 { return a + b; }
const result: i32 = add(2, 3);
fn main(): i32 { return result; }
```

Passing program must return 5. Comptime `throw` must stop with `E6005`.

Commands:

```bash
cmake --build build -j --target test-comptime-eval
ctest --test-dir build --output-on-failure
```

## Docs To Update

- `impl-status.md`: `comptime` evaluation and `const fn` move from Spec only to
  Working.
- `docs/11-comptime.md`: status header updated.
- `docs/roadmap.md`: F-15/F-16 complete.
