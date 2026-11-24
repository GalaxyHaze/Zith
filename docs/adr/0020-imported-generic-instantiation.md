# Imported Generic Structs And Methods Reuse The Local Instantiation Path

Generic declarations imported through a module alias must be reified from the declaring module's template, not from the current module snapshot. Qualified struct literals, type annotations, field defaults, and generic methods on imported concrete receivers use the same `StructType.args` and generic instantiation machinery as local generics; missing method type arguments are inherited from the concrete receiver. This is required before `DynArray<T>` or an expanded `HashMap<K, V>` becomes a consumable stdlib surface.

Status: accepted

Considered options:

- A separate imported-template instantiation path was rejected because it would duplicate reification, bound propagation, `bindCall`, and cache identity handling, and would drift from the local generic behavior.
- A docs-only contract claiming generic maps are consumable was rejected until the compiler can instantiate them from another module.

Consequences:

- `import soon/collections/hash_map` supports `hm.HashMap<i64,i64>{}`, field defaults from the declaring module, and `hm.HashMap.init(lend map)` without repeating type args. Nested generic calls inside a generic body are concretized transitively, so `init<i64,i64>` calls `reserve<i64,i64>` rather than the abstract `reserve<T,T>`.
- `stdlib/soon/collections/hash_map.zithm` is now a consumable checked generic map: `examples/hash-map.zithm` passes `zithc check` and runs to exit 0, and both `tests/test-generic-hashmap.cpp` and `test-codegen` exercise an imported `HashMap<i64,i64>`.
- `DynArray<T>` shares the same path and is promoted: `examples/dyn-array.zithm` passes `zithc check` and runs to exit 0, and `test-codegen` exercises an imported `DynArray<i32>` end-to-end.
