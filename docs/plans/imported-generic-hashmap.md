# Imported Generic Hash Map Instantiation Plan

Status: complete. The imported generic `HashMap<K, V>` is consumable from a
consumer module, and `examples/hash-map.zithm` passes `zithc check` and runs to
exit 0. `DynArray<T>` shares the same path and is promoted: `examples/dyn-array.zithm`
passes `zithc check` and runs to exit 0, and `test-codegen` exercises an imported
`DynArray<i32>` end-to-end.

Objective: make generic declarations importable and usable from another module:
`hm.HashMap<i64, i64>{}`, field defaults from the declaring module, and generic methods such as `hm.HashMap.init(lend map)` without repeating type arguments. Validate with `examples/hash-map.zithm` and `tests/test-generic-hashmap.cpp`. `DynArray<T>` is out of scope.

Preconditions:

- Working directory is `/home/diogo/Zith`.
- `./build/zithc --include stdlib --no-cache check examples/hash-map.zithm` currently fails with `E2007` and `E3010`.
- A minimal imported module reproduces the same failures without the real `hash_map.zithm`.
- The existing `tests/test-generic-hashmap.cpp` passes for in-session source.

Files likely modified:

- `src/sema/sema-literal.cpp`
- `src/sema/sema-type.cpp`
- `src/sema/sema-method.cpp`
- `src/session/frontend-symbol-resolution.cpp`
- `tests/test-generic-hashmap.cpp`
- `examples/hash-map.zithm` if its API or invocation needs adjustment
- `docs/implementation-debt.md`
- `docs/impl-status.md`
- `docs/20-standard-library.md`

Forbidden actions:

- Do not touch `stdlib/std/collections/hash_map_u64.zithm`.
- Do not create a separate imported-template instantiation path.
- Do not add raw debug printing.
- Do not revert unrelated user changes.

## Step 1 - Resolve Qualified Generic Struct Literals

Goal: `hm.HashMap<i64, i64>{}` reaches the declaring module's generic template.

1. In `src/sema/sema-literal.cpp`, when a literal has `genericArgs` and a dotted name, resolve the qualified declaration through the import target before calling `instantiateTypeExpr`.
2. In `src/sema/sema-type.cpp`, extend `instantiateTypeExpr` (or add a qualified overload) to accept the declaring module's `snapshot.declarations()` and `findDeclarationForResolved`.
3. Keep `StructType.args` populated exactly as in local generic reification.
4. Add a focused test that imports a minimal generic module and creates `hm.HashMap<i64,i64>{}`.

Success: the `E2007` no longer appears on the generic struct literal.

## Step 2 - Reuse Imported Field Defaults

Goal: `{}` accepts defaults declared by the imported struct.

1. Locate the current default lookup path used by `inferStructLiteral` and `resolveGenericStructLiteral`.
2. Make the default expression resolve against the declaring module's frontend snapshot, not only the current module.
3. Test `hm.HashMap<i64,i64>{}` with defaults; the `E3001` missing-field errors must disappear.

## Step 3 - Resolve Imported Generic Method Calls

Goal: `hm.HashMap.init(lend map)` and `hm.HashMap.init<i64,i64>(lend map)` both work.

1. In `src/sema/sema-method.cpp`, find `ownerTemplate` from the method's declaring module when `snapshot.declarations()` does not contain the owner.
2. Trust `StructType.args` on the concrete receiver when it is available.
3. Make explicit `genericArgs` lower against the declaring module if needed.
4. Add a test calling `init`, `destroy`, and at least one bounded method such as `contains`/`put` on an imported `HashMap<i64,i64>`.

Success: no `E3010` for implicit or explicit method type arguments.

## Step 4 - Integration And Docs

Goal: the generic map is consumable from the CLI and documented accurately.

1. Make `examples/hash-map.zithm` pass `zithc check` and, when runnable, exit 0.
2. Extend `tests/test-generic-hashmap.cpp` with the import-based cases.
3. Update `docs/impl-status.md`, `docs/20-standard-library.md`, and `docs/implementation-debt.md` after successful verification.
4. Close the stale residual note about `self->table->occupied`; replace it with the actual imported-instantiation dependence.

Success criteria:

- `./build/zithc --include stdlib --no-cache check examples/hash-map.zithm` passes.
- `./build/tests/test-generic-hashmap` passes.
- At least one CTest run exercises an imported generic `HashMap<K, V>`.
