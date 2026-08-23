# Sema Literal Checks And Lowering Hardening

This file records the diagnostics added for struct literals and struct-name
field access, plus test gotchas around persistent cache and the modern
pipeline. It is useful when touching `src/sema/sema-modern.cpp`,
`src/sema/hir-lower-modern.cpp`, or test suites that run full pipelines.

## Problem Background

The bug report described two failures in `build/main.zith`:

1. `pair.first` and `pair.second`, where `pair` is the struct name itself, was
   accepted by sema. The subsequent `printf` call disappeared from the final
   executable because lowering produced no value for the struct-name base.
2. A struct literal that omitted a field without a default was accepted even
   though the field was mandatory. The missing field was silently zero-filled
   by later stages or simply lost.

Both problems came from the same class of compiler defect: sema allowed an
invalid expression that lowering could not represent, and lowering chose to
emit less HIR instead of failing the pipeline. The fix is a combination of
sema diagnostics and lowering invariants.

## Struct Literal Fields

Struct literal inference now requires every field without a default, whether
the literal is positional or named. Fields with a declaration default remain
omittable; the implementation materializes them during lower.

The check runs after the existing field loop in
`PerModuleSema::inferStructLiteral`. It uses `seen[]` and `findFieldDefault`,
so it does not need per-field spans and works for reordered named literals.

The diagnostic is a `TypeMismatch` with a fixed message:

`missing field '<name>' in struct literal; add a value or a field default`

The existing placeholder check for `_` is unchanged. A field with a default
may still be omitted, including a literal such as
`Pair{right: 9, left: _}` when both fields have defaults.

## Struct Name Field Access

`Pair.first` where `Pair` is a struct declaration is now rejected in
`PerModuleSema::inferField`. The guard looks at the resolved name for the
base:

- if the base resolves to a `Declaration` whose `declKind` is `Struct`, report
  `TypeMismatch`;
- otherwise continue with the existing path.

Enum variant accesses and import aliases remain unaffected because their bases
resolve through different paths. The diagnostic is fixed:

`struct name '<name>' cannot be used as a value in field access; use a value such as 'p.first'`

## Lowering Hardening

`HirLowerModern::lowerCall` now returns `kInvalidHirExpr` as soon as an
argument fails to lower instead of emitting a call with a shortened argument
list. The original code pushed only valid arguments and continued, so a malformed
argument left a `HirCall` whose later arguments were silently missing.

`HirLowerModern::lowerStatement` now reports `InvalidIR` for an expression
statement that is mandatory and fails to lower. It skips:

- void-typed expressions, which legitimately have no HIR value;
- error-typed expressions, because sema already reported the cause;
- pipelines where `diags_` already has errors, to avoid cascading noise;
- statements such as `If`, `While`, `For`, `Block`, and `MacroCall`, which are
  lowered by their own cases and may return `kInvalidHirExpr` for legitimate
  control-flow reasons.

The lowering hardening is intentionally conservative. Calls, returns, and
loops already tolerate expressions that cannot be emitted when sema failed;
the new checks only stop clearly invalid mandatory expressions from being
discarded silently.

## Reproducers

Invalid struct-name access:

```zith
struct Pair { first: i32, second: i32 }
fn take(a: i32, b: i32) {}
fn main() {
    take(Pair.first, Pair.second);
}
```

Invalid missing positional field:

```zith
struct Pair { left: i32, right: i32 }
fn main() {
    var p: Pair = Pair{1};
}
```

Invalid missing named field:

```zith
struct Pair { left: i32, right: i32 }
fn main() {
    var p: Pair = Pair{right: 9};
}
```

Valid defaulted literal:

```zith
struct Pair { left: i32 = 3, right: i32 = 4 }
fn main() {
    var p: Pair = Pair{right: 9};
}
```

## Test Coverage

`tests/test-sema.cpp` covers:

- positional missing field without default fails with `TypeMismatch`;
- named missing field without default fails with `TypeMismatch`;
- named omitted field with default succeeds;
- struct name plus field fails with `TypeMismatch`.

`tests/test-hir-lower-modern.cpp` covers a call with an unlowerable argument.
The reproducer uses an invalid char escape, which sema accepts and lowering
rejects with `InvalidEscape`; the test asserts the pipeline fails and no HIR
call survives.

## Test Gotchas

- `test-examples` can fail on "the HIR dump lands on stderr" after a hot build
  because `build/examples-run/` keeps stale cache. Running with `--verbose`
  still prints HIR in that directory; remove `build/examples-run` and rerun
  the suite.
- The imported jump/dock test in `test-frontend-modern-pipeline` fails on HEAD
  itself with "jump to undefined marker 'target'". It is a pre-existing
  inability to type-check marker bodies in imported macro expansions, not a
  regression from the struct/field/lowering changes.
- A clean HEAD checkout needs `mio` before configure. Copy
  `build/_deps/mio-src` and `mio-build` into the new build tree, then use
  `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` when network is restricted.

## Verification Commands

Useful checks after touching this area:

```bash
cmake --build build -j
./build/test-sema
./build/test-hir-lower-modern
ctest --test-dir build --output-on-failure
```

After removing stale `build/examples-run`, `ctest` is expected to pass all
tests except the pre-existing imported jump/dock failure in
`test-frontend-modern-pipeline`.

## Code Path Notes

`inferStructLiteral` is the only modern sema entry point for struct literal
expressions. It resolves by `expr.text` plus optional generic args, then models
seen fields as `std::vector<bool>`. Reusing that vector for the missing-field
report is deliberate: positional literals, named literals, duplicate fields,
and `_` placeholders all share one normalization path before the final
validation loop.

`findFieldDefault` scans `snapshot.declarations()` for the struct declaration
and returns the field's `defaultValue` expression id. It expects the struct
parameters to remain ordered; the per-field index is the same index used by
`st->fields` and `st->field_names`.

`inferField` has three pre-existing special cases before generic struct field
inference:

1. the whole field expression may resolve as an import member, so the base
   import alias is not interpreted as a value;
2. an enum variant access is handled by `enumVariantType`;
3. a struct declaration base is now rejected.

The struct-name guard intentionally sits after enum handling and before
`inferExpr(base)`. Putting it before `inferExpr` avoids secondary errors such
as "cannot infer type" or "field access on non-struct type" for `Pair.first`.

## Diagnostics And Error Codes

Borrowed messages use `diagnostics::err::TypeMismatch`, the same code as other
sema type errors. The missing-field diagnostic intentionally does not point at
the omitted field, because the frontend stores no placeholder node for an
omission; the literal span is the best available location without adding a new
AST node or span table.

The structure-name diagnostic points at the whole field access expression
`Pair.first`, not just the base name. This keeps the fix minimal while making
the type-as-value problem visible to the user.

## Lowering Interaction With Sema

Lowering runs after sema has typed every expression. A `kInvalidHirExpr`
return means "this AST node has no representable HIR value". It is not always
an error:

- `while` and `for` intentionally produce control flow and no value;
- a block whose last statement is a binding produces no last value;
- `Range` is only lowered from `when` conditions;
- a macro expansion may be absent until expansion fills it.

For this reason the expression-statement guard checks the lowered expression's
type instead of treating every invalid expr as a failure. Void and error types
are excluded, and `If`/`While`/`For`/`Block`/`MacroCall` are lowered through
dedicated cases that already decide whether their result is meaningful.

For calls, the argument loop is the boundary where malformed operands used to
be skipped. Bailing out at the first invalid argument preserves the invariant
that `HirCall.operands` and `HirCall.arg_types` always describe the same
argument list.

## Cache And Reproducibility Notes

`build/examples-run` is the test workdir used by `test-examples`. The suite
copies examples there and runs `zithc` repeatedly. Persistent cache and target
artifacts are written next to each source. When these survive from a different
binary version, the `--emit-hir` path can be skipped without a visible
diagnostic, which produced the confusing `test-examples` failure.

Manual reproductions in `/tmp` are safer than running inside `build/`
because each new repro starts with a clean cache. This also applies when
testing diagnostic changes: an old cached artifact for the same source path
can hide the new error.

The imported jump/dock failure is a separate known limitation. It happens with
an unmodified checkout, so it should be fixed in a dedicated issue/commit and
not treated as part of this sema/lower hardening work.

## Future Work

If per-field source locations are later needed for missing fields, the
frontend literal node could carry an explicit absent-field span or the sema
metadata could map declaration field indices back to spans from the struct
declaration. That would be a larger API change and was deliberately avoided
here because the current messages satisfy the acceptance criteria.

Similarly, the lowering hardening could be generalized to return a reason enum
from lowerers instead of a boolean invalid id, but that would touch every
`lower*` function and is out of scope for fixing these two user-visible bugs.
