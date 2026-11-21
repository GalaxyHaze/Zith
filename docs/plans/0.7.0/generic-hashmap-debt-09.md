# Generic Hash Map Debt 09 Implementation Plan

Objective: close implementation-debt item 9 by making nested generic struct
reification, concrete generic type identity, generic bound propagation, and
generic calls inside generic functions work in Zith--, then make
`stdlib/std/collections/hash_map.zithm` pass `zithc check` without changing its
public API.

Preconditions:

- Working directory is `/home/diogo/Zith`.
- Build directory `/home/diogo/Zith/build` is configured with CMake.
- `/home/diogo/Zith/build/zithc` is the local compiler binary.
- The repo follows `AGENTS.md`: no raw debug printing, no exceptions, no RTTI,
  clang-format rules, and tests registered through `add_zith_test`.

Files that will be modified:

- `/home/diogo/Zith/src/sema/modern-types.hpp`
- `/home/diogo/Zith/src/sema/modern-types.cpp`
- `/home/diogo/Zith/src/comptime/generic-instantiate.cpp`
- `/home/diogo/Zith/src/sema/sema-method.cpp`
- `/home/diogo/Zith/src/cache/cache-types.hpp`
- `/home/diogo/Zith/src/cache/artifact-builder.cpp`
- `/home/diogo/Zith/src/session/persistent-cache.cpp`
- `/home/diogo/Zith/tests/test-generic-hashmap.cpp`
- `/home/diogo/Zith/CMakeLists.txt`
- `/home/diogo/Zith/docs/implementation-debt.md`
- `/home/diogo/Zith/docs/impl-status.md`
- `/home/diogo/Zith/docs/20-standard-library.md`
- `/home/diogo/Zith/docs/Zith---implementation.md`
- `/home/diogo/Zith/docs/adr/0019-concrete-generic-type-identity.md`

Forbidden actions:

- Do not touch `stdlib/std/collections/hash_map_u64.zithm`.
- Do not change the public API of `stdlib/std/collections/hash_map.zithm`.
- Do not run `git reset --hard`.
- Do not revert unrelated user changes.
- Do not add raw `printf`, `std::cerr`, or `fprintf(stderr, ...)`.
- Do not use `memsearch`; use `python3 /home/diogo/Zith/scripts/rag.py search`
  instead.

## Step 1 - Add Concrete Generic Type Arguments To `StructType`

Goal: every reified struct carries its concrete type arguments as metadata,
from both sema-side instantiation and comptime substitution.

Sub-steps:

1. Open `/home/diogo/Zith/src/sema/modern-types.hpp`.
2. Add an `args` member to `struct StructType` with type
   `memory::DynArray<TypeId> &`.
3. Add a `struct_args` member to `struct TypeTable::Entry` with type
   `memory::DynArray<TypeId> * = nullptr`.
4. Change `TypeTable::internStruct` to accept an optional
   `memory::DynArray<TypeId> *struct_args = nullptr`.
5. In `/home/diogo/Zith/src/sema/modern-types.cpp`, create a `makeTypeStorage()`
   and store it in `entry.struct_args`, then set `entry.struct_ty->args` to that
   storage when `struct_args != nullptr` or to an empty storage otherwise.
6. Add a public accessor `[[nodiscard]] const StructType *struct_type(TypeId id)
   const noexcept;` already exists, so expose the args through the existing
   `struct_type` result; no new accessor is required.
7. Update every call site of `internStruct` so reified instances pass the
   concrete args:
   - `GenericInstantiationPass::substituteType` in
     `/home/diogo/Zith/src/comptime/generic-instantiate.cpp`: pass `args`.
   - `PerModuleSema::instantiateTypeExpr` struct branch in
     `/home/diogo/Zith/src/sema/sema-type.cpp`: pass the `args` vector after
     lowering.
   - `PerModuleSema::instantiateStructFromArgs` in
     `/home/diogo/Zith/src/sema/sema-type.cpp`: pass the `args` parameter.
   - Template registration. A generic template should store the pending
     generic parameter declarations for owner method resolution, not concrete
     args. The existing declaration registration in
     `/home/diogo/Zith/src/sema/sema-decl.cpp` stays with empty args until the
     concrete instance is created.
8. Run clang-format on the changed C++ files:

   `cmake --build /home/diogo/Zith/build --target fmt` or
   `/usr/bin/clang-format -i` plus the style from `.clang-format`.

9. Verify the build:

   ```
   cd /home/diogo/Zith
   cmake --build build -j
   ```

Expected output: the build exits 0, no `-Werror` warning is reported, and no
API compile error appears for `internStruct`.

Failure checks:

- If `internStruct` has callers you did not update, the compiler reports
  `too few arguments to function call`. Update the reported caller.
- If `StructType` initialization order breaks, the compiler reports an
  aggregate initialization error. Keep the new member in the declared order.
- If `cmake --build` produces unrelated errors already present in the tree,
  stop and report the output verbatim. Do not fix unrelated files in this step.

Success criteria:

- `cmake --build /home/diogo/Zith/build -j` exits 0.
- The new `StructType.args` and `TypeTable::Entry.struct_args` members compile.

## Step 2 - Centralize Reification In `substituteType`

Goal: `GenericInstantiationPass::substituteType` is the single helper that
reifies struct, alias, nominal, enum, and union types with substituted names,
substituted layouts, and stored type arguments.

Sub-steps:

1. Open `/home/diogo/Zith/src/comptime/generic-instantiate.cpp`.
2. Keep `concreteTypeName` for alias, nominal, and enum reification.
3. Rewrite `concreteStructName` so the concrete name is built from the
   substituted field types after substitution. For the field at index `i`,
   compute `substituted = substituteType(field, args)` and append
   `type_table_.typeToString(substituted)`. Do not read the pre-substitution
   origin from `genericParamOrigin` for the name.
4. In the `TypeKind::Struct` branch of `substituteType`, compute the fields
   first, then the concrete name from those substituted fields, then call
   `type_table_.internStruct(concrete, fields, &st->field_names, &meta, &args)`.
5. In the `TypeKind::Union` branch, compute members first, then call
   `type_table_.internUnion(concrete, members, ut->is_tagged)`. Unions keep
   concrete type args through their members; if the method resolver needs them,
   copy the `args` vector into a helper in later analysis rather than adding a
   new union field in this step.
6. Keep alias and nominal `substituteType` behavior, and pass through the
   helper for reification from `PerModuleSema::instantiateTypeExpr` and
   `PerModuleSema::instantiateStructFromArgs` where those functions still
   construct concrete names and layouts directly.
7. Do not let the central helper orphan the `named_registry_` entries. Every
   reified type must call `registerNamed` for its concrete name before return.
8. Run `cmake --build /home/diogo/Zith/build --target fmt`.

Verify with a fresh compiler run:

```
cd /home/diogo/Zith
cmake --build build -j
```

Expected output: build exits 0.

Failure checks:

- If an existing cache artifact now has a stale type name, the build may report
  a cache mismatch. Clear only the generated cache under `/tmp` or the build's
  `.zith-cache` before a clean retry; do not delete source files.
- If `substituteType` recursion changes named type lookup behavior, run the
  generics mono test and inspect any diagnostic it reports.

Success criteria:

- `cmake --build /home/diogo/Zith/build -j` exits 0.
- A probe containing `struct Entry<K, V>` inside `HashMap<K, V>` with field
  `table: ?*Entry<K, V>` no longer renders `Entry<T, T>`.

## Step 3 - Serialize And Hydrate Struct Type Arguments In The Cache

Goal: a cached artifact that contains `HashMap<u64, u64>` preserves the
concrete type arguments on `CompactStructDef` and hydrates them after a cold
cache read.

Sub-steps:

1. Open `/home/diogo/Zith/src/cache/cache-types.hpp`.
2. Add `std::vector<uint32_t> type_args;` to `struct CompactStructDef`.
3. Open `/home/diogo/Zith/src/cache/artifact-builder.cpp`.
4. In the loop that builds `CompactStructDef`, after copying field types, copy
   the reified `StructType.args` through `internType` and store the ids in
   `cdef.type_args`.
5. Open `/home/diogo/Zith/src/session/persistent-cache.cpp`.
6. In the struct-definition hydration loop, after restoring the struct fields,
   convert each `type_args` id through `compactType` and store the resulting
   concrete arguments in the hydrated modern type metadata. The legacy
   `types::TypeIntern` side may store the arguments in a parallel vector that
   the modern `TypeTable` consumes when the artifact is promoted.
7. Add a cache round-trip test in `/home/diogo/Zith/tests/test-generic-hashmap.cpp`
   with two sessions: compile `HashMap<u64, u64>` with a persistent cache
   directory, remove the in-memory result, compile again with the same cache
   directory, and assert the hydrated result still reports
   `HashMap<u64, u64>` and its `Entry<u64, u64>` type args.
8. Run `cmake --build /home/diogo/Zith/build --target fmt`.

Verify:

```
cd /home/diogo/Zith
cmake --build build -j
./build/tests/test-cache
./build/tests/test-generic-hashmap
```

Expected output: both tests exit 0.

Failure checks:

- If `CompactStructDef` consumers do not tolerate the new field, the compiler
  reports an aggregate initialization or deserialization error. Update those
  consumers.
- If cold/warm hydration loses type args, the new test reports a mismatch.
  Inspect the point where `struct_tids` is restored and where `mTypes.addField`
  runs; do not change the `CompactType` wire format without also updating the
  reader.

Success criteria:

- `./build/tests/test-cache` exits 0.
- `./build/tests/test-generic-hashmap` includes a passing cold/warm cache
  assertion.

## Step 4 - Use `StructType.TypeArgs` In Method Resolution

Goal: method calls on concrete reified structs pass their stored concrete
owner arguments to the generic callee without parsing the type name or
scanning substituted fields.

Sub-steps:

1. Open `/home/diogo/Zith/src/sema/sema-method.cpp`.
2. Replace the heuristics around lines 803-877 that reconstruct
   `inherited_args` from `owner_st->fields`, `owner_ut->members`, or the enum
   name with a single source:
   - For `TypeKind::Struct`, read `owner_st->args` and copy it into
     `inherited_args`.
   - For union, if a future reified union needs args, use a stored metadata
     accessor; in this step, keep the current members-based behavior only when
     no better accessor exists.
   - For enum, keep the current name-parse fallback in this step because enums
     do not own type-arg metadata yet.
3. Preserve the `GenericParam` origin on `self` when resolving bounds. When
   reading `self->key` or another generic field owned by a function with
   `K: Hashable`, keep the owner's `GenericParam` type if the field type is
   itself generic. The stored `StructType.args` are for concrete reified
   receivers only.
4. Verify a call like `fn put<K: Hashable>(var self: lend HashMap<K, V>)`
   calling `contains<K, V>(view self, key)` resolves against the same `K, V`
   in context, and a concrete `HashMap<u64, u64>` call passes `u64, u64` to
   `contains`.
5. Run `cmake --build /home/diogo/Zith/build --target fmt`.

Verify:

```
cd /home/diogo/Zith
cmake --build build -j
./build/tests/test-generics-mono
./build/tests/test-generic-hashmap
```

Expected output: both tests exit 0.

Failure checks:

- If `inherited_args` is empty for concrete structs, the method call still
  fails with `E3011`. Print the receiver type and stored args in the test
  harness, not in the compiler.
- If `E3009` appears where `K: Hashable` should hold, the bound source was
  lost. Confirm the owner `GenericParam` is preserved and that
  `boundsForGenericParam` still sees it.

Success criteria:

- The generic-call-in-function probe passes without `E3011`.
- The trait-field probe passes without `E3001` or `E3009`.

## Step 5 - Propagate Bounds Through Generic Fields And Calls

Goal: reading a generic field from an owner inside a generic function keeps
the owner's `GenericBinding`, so `self->key.hash()` and `contains(...)` resolve
through `K: Hashable`.

Sub-steps:

1. Open `/home/diogo/Zith/src/sema/sema-method.cpp`.
2. Inspect `inferMethodCall` and `boundsForGenericParam` for the path that
   reaches a field type after `self->key`. When the field is a `GenericParam`
   from the current owner declaration, carry that parameter's origin into the
   receiver expression type used for method resolution.
3. Add the helper path to `GenericInstantiationPass::substituteType` so a
   generic parameter nested under pointer, optional, array, slice, or other
   structured field still resolves through the same owner origin when the
   reified struct is substituted.
4. Do not remove `GenericBinding` as the source of truth. It stays authoritative
   for bounds; `StructType.args` are concrete metadata, not bound declarations.
5. Run `cmake --build /home/diogo/Zith/build --target fmt`.

Verify all probes:

```
cd /home/diogo/Zith
cmake --build build -j
./build/tests/test-generic-hashmap
```

Expected output: the test executable passes the probes named
`constraint-propagation`, `trait-field`, `generic-call-in-function`,
`entry-table`, `nested-struct`, and `return-optional`.

Failure checks:

- If `E3009` points to the argument type instead of the call, print the
  receiver and field types before the bound check.
- If `E3001` persists, the method being called is not resolved against the
  trait bound. Inspect `boundsForGenericParam` for the exact declaration id of
  the current owner.

Success criteria:

- All six named probes pass.
- `HashMap<K: Hashable, V>` methods compile inside `HashMap<K, V>`.

## Step 6 - Promote Probes To A Dedicated Test

Goal: `/home/diogo/Zith/tests/test-generic-hashmap.cpp` is a registered,
deterministic test that covers all debt-9 probes and the checked stdlib
`hash_map.zithm`.

Sub-steps:

1. Create `/home/diogo/Zith/tests/test-generic-hashmap.cpp`.
2. Copy the nine probe sources from `/tmp/zith-generic-probes/` in a compact
   form: `basic.zithm`, `nested-struct.zithm`, `entry-table.zithm`,
   `trait-field.zithm`, `constraint-propagation.zithm`, `return-optional.zithm`,
   `equality.zithm`, `nested-generic.zithm`, and
   `generic-call-in-function.zithm`.
3. Assert accepted programs report zero unexpected diagnostics and rejected
   probes report the expected `E3009` diagnostic when a non-hashable key is
   used.
4. Add a compile-and-lower test that runs `zithc check` on
   `/home/diogo/Zith/stdlib/std/collections/hash_map.zithm` and asserts no
   `E2006`, `E3001`, `E3003`, `E3011`, or `E3009`.
5. Add a conservative execution assertion for `HashMap<u64, u64>` that stops at
   `HirLowered` unless `HirInterpreter` proves `calloc` and `free` externs are
   supported. Do not add a native codegen assertion.
6. Register the test in `/home/diogo/Zith/CMakeLists.txt` next to the existing
   generic tests with `add_zith_test(test-generic-hashmap
   tests/test-generic-hashmap.cpp)`.
7. Run `cmake --build /home/diogo/Zith/build --target fmt`.

Verify:

```
cd /home/diogo/Zith
cmake --build build -j
./build/tests/test-generic-hashmap
```

Expected output: the executable exits 0 and reports all probe assertions
passed.

Failure checks:

- If a probe produces a different diagnostic in this build, print the
  diagnostic and source span in the test failure. Do not weaken the expected
  diagnostic to make the test pass.
- If `zithc check` on `hash_map.zithm` fails after Step 5, the test failure is
  a real sema regression. Re-run Steps 4 and 5 before changing the test.

Success criteria:

- `./build/tests/test-generic-hashmap` exits 0.
- `CMakeLists.txt` contains `add_zith_test(test-generic-hashmap
  tests/test-generic-hashmap.cpp)`.

## Step 7 - Update Docs And Debt Status

Goal: debt item 9 is marked planned/implemented, standard-library status
reflects a checked generic `HashMap<K, V>`, and implementation notes explain
structural concrete generic identity and bounds propagation.

Sub-steps:

1. Open `/home/diogo/Zith/docs/implementation-debt.md`.
2. Update item 9 with an implementation status. When Steps 1-6 pass, mark it
   `Resolution: planned` during implementation and `Resolution: implemented`
   after the final test run. Keep the criteria and probes as the verification
   evidence.
3. Open `/home/diogo/Zith/docs/impl-status.md`.
4. Update the `Generic instantiation` row to `Working (full first-class
   structs)` or the exact status language already established in the file, and
   mention nested struct reification, concrete type args, and bound
   propagation through generic fields.
5. Update the `Stdlib collections` row so `hash_map` changes from
   `proposed draft` to `checked stdlib`, with `hash_map_u64` remaining the
   shipped runtime surface unless the interpreter assertion in Step 6 proves
   otherwise.
6. Open `/home/diogo/Zith/docs/20-standard-library.md`.
7. Update the `std/collections/hash_map` section so it no longer says nested
   generic reification and bound propagation block the module. Keep the
   `u64 -> u64` example as the concrete shipped surface.
8. Open `/home/diogo/Zith/docs/Zith---implementation.md`.
9. Insert a short section under Sema or Comptime with:
   - concrete generic type identity is structural and name-derived;
   - `StructType` stores concrete type arguments;
   - method resolution uses those arguments instead of field scans or name
     parsing;
   - `GenericBinding` stays the source of truth for bounds;
   - cache serializes concrete type arguments.
10. Run `cmake --build /home/diogo/Zith/build --target fmt` if any C++ files
    changed after the previous steps, otherwise verify docs with a simple read.

Expected output: the four doc files reflect the implementation and no stale
blocked-generics language remains.

Failure checks:

- If `docs/20-standard-library.md` still says `does not zithc check`, the
  update was incomplete. Re-read the section and replace that sentence.
- If `docs/impl-status.md` has another generics row or table location, update
  the row that names `Generic instantiation` and `Stdlib collections`.

Success criteria:

- `rg -n "proposed draft" docs/20-standard-library.md` no longer refers to
  `hash_map`.
- `rg -n "blocked" docs/20-standard-library.md docs/impl-status.md
  docs/implementation-debt.md` no longer refers to generic `HashMap`.

## Final Acceptance

Run the focused tests and the real CLI check:

```
cd /home/diogo/Zith
cmake --build build -j
./build/tests/test-generic-hashmap
./build/tests/test-generics-mono
./build/tests/test-generic-constraints
./build/zithc --include stdlib check stdlib/std/collections/hash_map.zithm
ctest --test-dir build -R 'generic|hash' --output-on-failure
```

Expected output:

- `./build/tests/test-generic-hashmap` exits 0.
- `./build/tests/test-generics-mono` exits 0.
- `./build/tests/test-generic-constraints` exits 0.
- `./build/zithc --include stdlib check stdlib/std/collections/hash_map.zithm`
  exits 0 with no error diagnostics.
- CTest reports 100% tests passed for the selected generic/hash tests.

Then run the full suite:

```
cd /home/diogo/Zith
ctest --test-dir build --output-on-failure
```

Expected output: CTest reports 100% tests passed. If any unrelated test fails,
report the failure verbatim and do not claim full acceptance.
