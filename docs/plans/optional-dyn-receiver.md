# Optional `dyn Trait` Receivers and Fields

Status: complete. W1 (payload erasure before the `Some` wrap, including the
assignment site), W2 (optional receiver unwrap plus the narrowing proof), W3
(no `ret void` in a dead value block) and W4 (`?opaque` slots) are implemented
and locally verified. The niche representation (decision 3) landed as ADR
0049, so `?dyn T` is layout-identical to `dyn T`. Acceptance 1-6 hold; the
only red CTest case is the unrelated in-progress `soon` scalar-math test.

Size: one compiler unit that spans sema method lookup, HIR lowering of the
optional coercion, and codegen. It unblocks the stdlib allocator-ownership work
that retains an allocator as a `?dyn Allocator` field.

## Problem

`Zith--` supports `dyn Trait` as a struct field, a local, a parameter,
and a return type, and dispatches methods through the embedded vtable. The
optional form `?dyn Trait` is broken end to end. The motivating consumer is the
standard-library allocator retention field:

- `stdlib/std/collections/hash_map.zith:18` declares `allocator: ?dyn Allocator = null,`.
- `stdlib/std/collections/hash_map.zith:107` writes `self->allocator = allocator;`.
- `stdlib/std/collections/hash_map.zith:70` and `:118` read `self.allocator` back.

This plan does not change `dyn Trait`. That path already passes `zithc check`
and `zithc run`. It fixes the optional receiver and the optional field.

## Evidence (defect matrix)

Every probe below uses this base and runs `zithc --no-cache` from the repo root.

```
trait Counter { fn get(self): i32; }
struct Cell { v: i32 }
implement Cell as Counter { fn get(self): i32 { return self.v; } }
```

| Probe | check | run |
| --- | --- | --- |
| `dyn Counter` field, local, param, return, dispatch | pass | pass |
| `?dyn Counter` field assigned from a `dyn` value, then `raw` dispatch | pass | pass |
| `let c: ?dyn Counter = d` where `d` is already `dyn` | pass | pass |
| `?dyn Counter` field or local, dispatch with no narrowing | E3001 | E3001 |
| `?dyn Counter` local, early-return `is null`, then `.get()` | E3001 | E3001 |
| `?dyn Counter` local, `if (c is null) { } else { c.get() }` | pass | E5001 |
| `?dyn Counter` struct-literal field from a concrete value | pass | LLVM abort |
| `?dyn Counter` local binding from a concrete value | pass | LLVM abort |
| `?dyn Counter` parameter or return from a concrete value | pass | LLVM abort |
| `raw` on a `?dyn` local or field | pass | LLVM abort |
| `if (x == 0) { return 1; } else { return 2; }` on plain `i32` | pass | E5001 |
| `?opaque` struct-literal field from a concrete value | E3001 | E3001 |

Three distinct defects hide behind "`dyn Trait` in a struct field is broken".

## Root causes

### R1 - the optional coercion wraps `Some` before erasing the payload

When a concrete value is stored into a `?dyn T` (or `?opaque`) slot, the
lowering wraps the value in `HirMakeSome` without first applying the dyn (or
opaque) erasure. The `dyn` test runs on the whole target type, which is
`Optional`, so it never fires.

- `src/sema/hir-lower-expr-agg.cpp:250-256` `coerceFieldValue` checks
  `kindOf(field_sema) == TypeKind::Dyn`. For a `?dyn` field the kind is
  `Optional`, so no `lowerCoerceToDyn` runs. `:277-282` wraps the concrete value
  in `Some` first.
- `src/sema/hir-lower-stmt.cpp:113-118` checks
  `kindOf(binding_sema) == TypeKind::Dyn`. For a `?dyn` binding the kind is
  `Optional`, so no `lowerCoerceToDyn` runs. `:100-110` wraps first.
- `src/sema/hir-lower-call.cpp:388-400` has the same shape for a `?dyn`
  parameter.
- `src/sema/hir-lower-modern.cpp:854-901` has the same shape for a `?dyn`
  return.

The malformed HIR stores a `Some` whose payload is the raw `Cell` aggregate into
a `{ {ptr, ptr}, i1 }` slot. The emitted IR for
`let c: ?dyn Counter = Cell{v:7}; return 0;` is:

```
store %zith.struct.1 { { { ptr, ptr }, i1 } { %zith.struct.0 { i32 7 }, i1 true } }
```

The payload is `%zith.struct.0 { i32 7 }`, not a two-word fat pointer. Selection
DAG aborts in `combineStore` with `Cannot implicitly convert a scalable size to
a fixed-width size in TypeSize::operator ScalarTy()`. The vtable global
`_zith.vtable.Counter.Cell` is never emitted for the optional case, which
confirms the erasure step was skipped.

### R2 - method lookup does not unwrap an optional `dyn` receiver

`PerModuleSema::inferMethodCall` (`src/sema/sema-method.cpp:119`) unwraps
`Optional` at `:183-201`, but it only adjusts the local `pointee` variable. The
dispatch test at `:203-204` reads the untouched `resolved_base`:

```
const TypeId resolved_base = resolve(base_type);
if (type_table.kindOf(resolved_base) == TypeKind::Dyn)
    return inferDynMethodCall(call, callee, resolved_base);
```

For `?dyn T`, `resolved_base` is `Optional`, so `inferDynMethodCall` never runs.
The call then falls back to reading `c.get` as a field.

- For a local receiver, `provenOptionalAggregatePayload`
  (`src/sema/sema-cast-coerce.cpp:102`) returns the `dyn` payload, and
  `src/sema/sema-index.cpp:324-340` sets the object type to `dyn`, then
  `:381-385` reports `E3001 field access on non-struct type having type 'dyn
  Counter'`.
- For a field receiver (`h.c.get()`), `provenOptionalAggregatePayload` returns
  `kInvalidTypeId` because it only handles locals (`resolved->local`), so the
  message names `'?dyn Counter'`.

`provenOptionalAggregatePayload` also excludes pointer payloads but has no case
for `Dyn`. A `dyn` payload is an aggregate, so it should follow the aggregate
path, not the pointer niche path.

### R3 - a dead merge block emits `ret void` in a value-returning function

When `if (cond) { return A; } else { return B; }` is used as a statement, both
branches terminate, so the merge block is unreachable. The implicit-return path
still terminates that block with a `HirRet` that has no value
(`src/sema/hir-lower-modern.cpp:854-901`). Codegen turns a value-less `HirRet`
into `CreateRetVoid` (`src/codegen/codegen-emit-stmt.cpp:119-120`), which fails
LLVM verification against an `i32` function:

```
bb3:                        ; No predecessors!
  ret void
```

This is not dyn-specific. `fn f(x: i32): i32 { if (x == 0) { return 1; } else
{ return 2; } }` fails the same way. Sema already rejects a genuine fall-through
(`E3001 function body can fall through without returning a value`), so a
value-less `HirRet` in a non-void function can only be produced for a block that
cannot be reached.

## Decisions (proposed)

These are the recommendations from the grilling round. Mark each as confirmed or
changed before implementation starts.

1. **Scope is the optional receiver `?dyn T`, not `dyn T`.** `dyn T` is the
   baseline that must not regress. Reversal: none, the evidence fixes the scope.
2. **The acceptance consumer is the stdlib `?dyn Allocator` field.** The fix is
   general for any `?dyn Trait`, with the hash-map allocator retention as the
   end-to-end test.
3. **Representation: niche, not a tag.** `?dyn T` uses the fat pointer itself,
   with a null data pointer as `None`, so `?dyn T` and `dyn T` are layout
   identical. This avoids extracting the payload at every read. Reversal: a
   tagged `{dyn, i1}` is simpler in codegen-type but forces payload extraction
   in the field, call, return, and assignment paths.
4. **Dispatch on `?dyn T` requires narrowing.** `.get()` on a possibly-null
   `?dyn T` is an error until the value is proven non-null with `is null`. This
   matches `?*T`. A dedicated diagnostic replaces the generic `E3001`.
5. **`provenOptionalAggregatePayload` covers field reads.** The proof must span
   any proven-non-null expression, not only a `LocalId`.
6. **Early-return and `else` narrowing behave the same.** Both must let the
   narrowed `?dyn T` receiver dispatch.

## Workstreams

### W1 - erase the payload before wrapping in `Some` (fixes R1)

Add one helper that erases a concrete value to the optional's inner `dyn` or
`opaque` payload, then wraps it in `Some`.

Add to `src/sema/hir-lower-modern.hpp` next to `lowerCoerceToOptionalDepth`:

```
hir::HirExprId lowerCoerceToOptionalErased(types::TypeId target_opt_hir,
                                           sema::modern::TypeId target_opt_sema,
                                           frontend::ExprId expression,
                                           hir::HirExprId value);
```

Implement it in `src/sema/hir-lower-expr-value.cpp` next to
`lowerCoerceToOptionalDepth` (`:1723`). Behavior:

1. Return `value` unchanged when `value` is invalid or either type id is
   invalid.
2. Resolve `optional = typeTable.optional(stripQualifiers(target_opt_sema))`.
   Return `kInvalidHirExpr` when it is null (caller falls back to the existing
   path).
3. Set `inner_sema = stripQualifiers(optional->inner)`.
4. When `kindOf(inner_sema) == Dyn`, set
   `value = lowerCoerceToDyn(inner_sema, expression, value, inner_sema)`.
5. When `kindOf(inner_sema) == Opaque`, set
   `value = lowerCoerceToOpaque(inner_sema, expression, value)`.
6. Otherwise return `kInvalidHirExpr` (no erasure needed).
7. Return `HirMakeSome{target_opt_hir, value}`.

Call it from the four sites before the plain `lowerCoerceToOptional` wrap, and
only when the payload is `Dyn` or `Opaque`:

1. `src/sema/hir-lower-expr-agg.cpp:277-282`, the struct-literal field wrap. The
   sema field type is `sema_struct_def->fields[slot_index]` and the field
   expression is `expr.operands[i]` (or the default id in the default loop at
   `:314-353`).
2. `src/sema/hir-lower-stmt.cpp:100-119`, the binding init. The sema target is
   `semaTypeOfLocal(statement.binding.id)`, the expression is
   `statement.binding.initializer`.
3. `src/sema/hir-lower-call.cpp:395-399`, the dyn-call optional argument wrap.
   Use the callee parameter's sema type and `expr.operands[index]`.
4. `src/sema/hir-lower-modern.cpp:875-880` and
   `src/sema/hir-lower-stmt.cpp:155-162`, the return coercion.

When the helper returns `kInvalidHirExpr`, keep the current
`lowerCoerceToOptional` path so non-dyn optional coercions are unchanged.

### W2 - unwrap the optional receiver in method lookup (fixes R2)

1. In `src/sema/sema-method.cpp`, after the `resolved_base` computation at
   `:203`, add: when `kindOf(resolved_base) == Optional` and the optional's
   inner resolves to `Dyn`, take `inner = resolve(optional->inner)` and return
   `inferDynMethodCall(call, callee, inner)`. This is the whole dispatch fix for
   both local and field receivers.
2. Gate the dispatch on a non-null proof per decision 4. When the receiver is a
   local, require `typed_map.provenNonNullExprs.contains(local)`. When the
   receiver is a field or another expression, require the same proof that
   `provenOptionalAggregatePayload` will use (see step 3). Without a proof,
   report a dedicated diagnostic, for example
   `E3001 cannot call 'get' on a possibly-null 'dyn Counter'; narrow it after 'is null'`.
3. In `src/sema/sema-cast-coerce.cpp:102`, extend
   `provenOptionalAggregatePayload` so a non-local expression can be proven
   non-null. The minimal change: accept the `LocalId` proof that
   `src/sema/sema-control.cpp:703-825` already records in
   `typed_map.provenNonNullExprs` for the early-return and `else` shapes, and
   drop the pointer-only exclusion for `Dyn` payloads so a `dyn` payload follows
   the aggregate path.
4. Record the receiver payload read so HIR lowers the receiver as the `dyn` fat
   pointer. Add the receiver expression id to
   `typed_map.optionalPayloadReads` with the `dyn` payload type, then in
   `src/sema/hir-lower-call.cpp:342` lower the dyn-call receiver through the
   same `optionalPayloadReads` extraction that
   `src/sema/hir-lower-expr-value.cpp:189-196` already uses for a named read.
   With the niche decision 3, the extraction is a no-op at the LLVM level, so
   this step only has to keep HIR and sema type-consistent.

### W3 - do not emit `ret void` in a non-void function (fixes R3)

This is a general fix and must land independently of the dyn work.

Preferred fix at the HIR boundary, in `src/sema/hir-lower-modern.cpp:854-901`:
when `current_fn_->return_type` is not `void` and there is no return value
expression, do not emit a value-less `HirRet`. Emit a terminator that models
unreachable control. The function reaches this branch only when the trailing
block cannot be reached, because sema rejects a genuine fall-through.

If no unreachable HIR node exists, use the existing `hir::HirRuntimePanic`
(`src/hir/hir-expr.hpp:442`) with a dedicated code, or add a minimal
`HirUnreachable` node and lower it to `CreateUnreachable`.

Alternative fix at the codegen boundary, in
`src/codegen/codegen-emit-stmt.cpp:119-120`: when `ret.value == kInvalidHirExpr`
and the enclosing function's return type is not void, emit
`builder_.CreateUnreachable()` instead of `builder_.CreateRetVoid()`. This is
smaller but leaves the value-less `HirRet` in the HIR. Prefer the HIR fix so the
HIR verifier and every backend see the same shape.

Do not weaken the sema fall-through check at
`src/sema/sema-state.cpp` to "fix" this. The check is correct.

### W4 - optional `opaque` field erasure (fixes the parallel defect)

The same R1 shape applies to a `?opaque` struct-literal field
(`struct H { c: ?opaque }` with `H{ c: Cell{v:7} }` is rejected `E3001`). The
`lowerCoerceToOptionalErased` helper from W1 already covers `Opaque`, so route
the struct-literal field and binding sites through it. Confirm the sema-side
field coercion also accepts a concrete value into a `?opaque` field the way it
accepts one into an `opaque` field.

## Tests

Add to `tests/test-codegen.cpp` (registered at `CMakeLists.txt:551`). Use the
existing `CodegenTest` harness (`tests/test-codegen.cpp:26`), which checks
`ok`, `errorCount == 0`, and `exitCode`.

1. `test_optional_dyn_local_binding_runtime`: `let c: ?dyn Counter = Cell{v:7};
   if (c is null) { return 1; } let r = raw c; return r.get();` expects exit `7`.
2. `test_optional_dyn_struct_field_runtime`: a `?dyn Counter` field set from a
   concrete value, read back with a narrowing check, dispatch returns `7`.
3. `test_optional_dyn_field_dispatch_narrowed`: `if (h.c is null) { return 1; }
   return h.c.get();` expects exit `7`.
4. `test_optional_dyn_return_and_param_runtime`: a function returns
   `?dyn Counter`, a caller narrows and dispatches, exit `7`.
5. `test_optional_dyn_without_narrowing_diagnostic`: dispatch on `?dyn` without
   a proof reports the dedicated diagnostic and no LLVM abort.
6. `test_if_else_both_return_runtime`: `fn f(x: i32): i32 { if (x == 0) {
   return 1; } else { return 2; } }` returns `2` for `x == 3`. This is the W3
   regression test and does not need dyn.

Add a sema-only test to `tests/test-optional-slice.cpp` (the
`test_nullable_pointer_narrowing` block at `:161`) that accepts a narrowed
`?dyn` dispatch for both the early-return and the `else` shapes.

Add a stdlib acceptance test: a program that builds a `HashMap<i64, i64>`,
inits it with `HeapAllocator`, inserts via `put`, reads via `get`, and destroys
it, expecting exit `0`. `examples/hash-map.zith` already covers init and
destroy; extend the check to `put` and `get`.

## Acceptance

1. `agent-run -t 900 -c 0 -- cmake --build build -j4` builds clean under
   `-Werror`.
2. `agent-run -t 900 -c 0 -- ctest --test-dir build --output-on-failure` reports
   `100% tests passed out of 48` or more, with the new tests listed.
3. Every probe in the defect matrix that is a `pass` stays `pass` for both
   `zithc check` and `zithc run`.
4. Every `?dyn Trait` probe that failed now either passes or reports the
   dedicated not-narrowed diagnostic. No probe aborts LLVM.
5. `?dyn` and `dyn` have identical size and alignment, verified with
   `@sizeOf(?dyn Counter) == @sizeOf(dyn Counter)` once the niche lands.
6. `agent-run -t 60 -c 50 -- ./build/zithc --no-cache run examples/hash-map.zith`
   exits `0`.

## Non-goals

- No change to `dyn Trait` without an optional layer.
- No mutable dyn receiver (`lend dyn TextSink`). That is tracked separately in
  `CONTEXT.md`.
- No general optional-of-aggregate redesign. The fix is limited to the `dyn`
  and `opaque` payloads that need erasure before the `Some` wrap.
- No change to the sema fall-through rejection.

## Glossary

Add to `CONTEXT.md` under the type terms, next to `Erasure support`:

**Optional dyn receiver**:
A `?dyn Trait` value used as a method-call receiver. It carries the same two
word fat pointer as `dyn Trait` and a null data pointer marks the `null` state,
so dispatch is reachable only after an `is null` narrowing proves the payload
present.
_Avoid_: nullable dyn, tagged dyn pointer
