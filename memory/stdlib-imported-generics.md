# Imported Generic Structs Are Now Consumable

Cross-module generic **struct** types were the main compiler blocker on the
stdlib surface. The imported generic path now reuses the local instantiation
machinery, so `import soon/collections/hash_map as hm` followed by
`hm.HashMap<i64, i64>{}` and `hm.HashMap.init(lend map)` compiles and runs.
ADR `docs/adr/0020-imported-generic-instantiation.md` is accepted and
`docs/plans/imported-generic-hashmap.md` is complete.

## What works now

```zith
// lib.zithm
pub struct Box<T> { value: T, }
```

```zith
// main.zithm
import lib as lb
fn main(): i32 { var b: lb.Box<u64> {}; return 0; }
```

- `lb.Box<u64>{ ... }` parses and reifies (the qualified dotted-name
  struct-literal path accepts a `<...>` list before `{`).
- Field defaults declared in the imported module are lowered with the declaring
  module's frontend/typed map.
- Generic methods on an imported concrete receiver (`hm.HashMap.init(lend map)`)
  inherit the receiver's type arguments, with or without explicit `<...>`.
- Nested generic calls inside a generic body are concretized transitively, so
  `init<i64,i64>` calls `reserve<i64,i64>` rather than the abstract
  `reserve<T,T>`.

## Fixes that landed

- `PerModuleSema::instantiateTypeExpr` / `instantiateTemplateFromArgs` /
  `findGenericTypeTemplate` reify from the declaring module's template and
  lower arguments against its snapshot (`src/sema/sema-type.cpp`).
- The frontend dotted-name struct-literal parser accepts `<...>` before `{`
  (`src/frontend/frontend-expr-primary.cpp`).
- Imported field defaults via `lowerFieldDefault(struct_name, field_index,
  declaring_module)` (`src/sema/hir-lower-expr-agg.cpp`).
- `GenericInstantiationPass::concretizeNestedInstances` plus
  `HirLowerModern::concretizeCallTarget` resolve nested generic callees to
  concrete instances (`src/comptime/generic-instantiate.*`,
  `src/sema/hir-lower-call.cpp`, `src/sema/hir-lower-modern.*`).
- Assignment to an optional-typed target wraps a bare `T` as `Some`, matching
  the binding/return paths, so `self->allocator = allocator` on a
  `?dyn Allocator` field is not read back as null
  (`src/sema/hir-lower-expr-value.cpp`).
- An instance body coerces returns against the reified result type (`?i64`
  rather than `?V`), so `get<K, V>: ?V` returns the optional aggregate
  (`src/sema/hir-lower-modern.cpp`).

## Gotcha: stale `genericParams_` shadowed `activeTemplateArgs_`

Reification runs `instantiateTemplateFromArgs` on the declaring module's sema,
but `currentDeclId_` can still point at the template declaration. In
`PerModuleSema::lowerBareTypeExpr` (Name case, `src/sema/sema-type.cpp`) the
`genericParams_[currentDeclId_]` lookup therefore held a stale
`T -> GenericParam` binding. When that lookup ran before `activeTemplateArgs_`,
it shadowed the concrete argument and left a reified struct field as `T`.

The visible symptom was `E3010 wrong generic argument count` at a call site
that returns the reified struct by value, because `typeContainsGeneric()`
reported the concrete struct as still generic and generic argument inference
ran on a type with no arguments. The trigger was an imported module whose file
stem equals the struct name (for example `Pair.zithm` defining `Pair<T>`), where
the declaring module's sema is reused for the caller's instantiation.

`activeTemplateArgs_` is now checked first and falls through to
`genericParams_` when empty. Regression:
`test-generics-mono.cpp::test_imported_generic_struct_returned_by_value`.

## Stdlib impact

- `soon/collections/hash_map.zithm` (`HashMap<K, V>`, `Entry<K, V>`) is a
  consumable checked generic map. `examples/hash-map.zithm` passes `zithc check`
  and runs to exit 0.
- `soon/collections/dyn_array.zithm` (`DynArray<T>`) shares the same path and is
  promoted: `examples/dyn-array.zithm` passes `zithc check` and runs to exit 0,
  and `test-codegen` exercises an imported `DynArray<i32>` end-to-end.
- The concrete `hash_map_u64.zithm` remains the shipped `u64 -> u64` map.
- `soon/result.zithm` uses the same imported-instantiation path for a generic
  `union` (not a struct): `Result<T, E>` and its `Ok`/`Err` helpers reify from
  the declaring module. The union must be `pub` or the import fails `E2001`.
  Result-only parameters bind from the expected result type (contextual
  inference), and `test-codegen` runs an imported `Result` end-to-end.
