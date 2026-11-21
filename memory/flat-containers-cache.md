# Flat Containers and Session Cache Playbook

Operational notes for future agents continuing the frontend/sema/cache
optimization work. This records what has already been migrated from
`std::unordered_map`/`std::unordered_set` to the project's flat containers, the
container API contracts known to matter in this codebase, and the current
session-cache behavior.

The current work is deliberately a consolidation pass: improve and clean what
already exists, do not add new compiler features or rewrite architectures.

## FlatMap / FlatSet API Contract

`FlatMap` lives in `src/memory/flat-map.hpp`; `FlatSet` lives in
`src/memory/flat-set.hpp`.

- `FlatMap<Key, Value, Hash, Eq>` supports string, string_view, arithmetic,
  enum, pointer, and custom class keys. The `Hash` template argument is the
  third parameter, matching the ordering used in call sites such as
  `SourceCatalog::by_key_`.
- `FlatMap::insert(key, value)` overwrites an existing entry and returns
  `Value&`. It takes the key by const reference, so callers passing a newly
  built key should keep a named copy around when the value must reference it.
- `FlatMap::get` returns `Value*`/`const Value*`; `FlatSet::contains` and
  `FlatSet::insert` return `bool`.
- Flat iterators are forward iterators over occupied slots. The `FlatMap`
  iterator dereferences to a temporary `pair<const Key&, Value&>`. Do not bind
  it with `const auto &[key, value]`; use `const auto item` and then
  destructure.
- Do not erase while iterating a flat container. Collect keys/values into a
  vector first, then erase after the loop.
- `FlatMap::erase` accepts a key, not an iterator.
- The string hash is transparent, so `FlatMap<std::string, ...>::get("literal")`
  and related string_view lookups work without converting to `std::string` first.

## Containers Already Migrated

- `SourceCatalog::by_key_` now uses `FlatMap<SourceKey, SourcePtr, SourceKeyHash>`,
  where `SourceKey` is a local custom key struct and `SourceKeyHash` is its custom
  hash. This exercised the generic-class-key path in `FlatMap`.
- `ModuleCache` storage uses `FlatMap` for artifacts, fingerprints, epochs,
  in-flight work, overlays, dependencies, and reverse dependencies.
- `Manifest` uses `FlatMap`/`FlatSet` for `by_path_` and `reverse_deps_`.
- `Store` invalidation and source discovery use `FlatSet` for deduplication.
- SEMa ownership/dead-state sets use `FlatSet<uint32_t>`:
  `movedLocals_`, `escapingPointerExprs_`, `escapingPointerLocals_`,
  `uninitializedLocals_`, and `preinitializedLocals_`.
- SEMa maps migrated to `FlatMap`: state machine ids, generic bindings,
  implement owner types, call targets, NRA narrowing facts, and
  `TypedMap`/`PerModuleSema` id-keyed tables.
- `GenericInstantiationPass::calls_`, macro expansion hygiene/scope maps, and
  `CompilationSession::mSnapshotDiagnosticFiles` are `FlatMap`.
- `ArtifactBuilder` interning tables are `FlatMap`.

The remaining `std::unordered_map` usage is telemetry
(`CompilationSession::getStageDurationsMs`) plus its test; it was intentionally
left unchanged.

## Known Compiler/Windows Build Gotchas After Migrating

- Loops that previously did `const auto &[k, v]` over `unordered_map` must not
  do the same over flat iterators. Clang with `-Werror` rejects the reference
  bind because dereference returns a temporary pair.
- Hash calls and keys must use the same type. For example, hashing a
  `string_view` against a `string` key is fine only because the default hash is
  transparent for strings; for integer keys with narrowing constants, keep the
  key type explicit (`const uint32_t key`) to avoid `-Wsign-conversion` in
  `operator[]`.
- `SraFacts::narrowing_facts_` is copyable but not pointer-stable after
  insertion/rehash. Code that needs both old and new facts copies the values
  before writing through the map.
- `ModuleCache::invalidateLocked` first collects eviction keys, then erases, so
  it does not invalidate iterators.

## Session Cache State

The CLI now exposes `--no-cache` to skip persistent `.zith-cache` reads and
writes for an invocation. In-memory frontend memoization still applies within
the session.

The bare `opaque` registry contract is the project-local `canonical-any` file
under the persistent cache root. `Store::assignCanonicalId` assigns and
persists a unique runtime tag per canonical type id on cold builds; artifacts
also serialize `canonical_mappings`, and warm hydration validates that table
against the reopened registry. A canonical field-order change yields a new
canonical id, so stale persisted tags are rejected with an actionable E2010
that identifies the canonical id and recommends deleting `canonical-any` plus
the cached `.zirl` artifacts, then rebuilding. Do not silently re-tag persisted
runtime ids when canonization evolves.

The canonical id rule is now a single shared function,
`types::canonicalTypeId` in `src/types/type-canonical.cpp`, used by HIR lowering
(`HirLowerModern::canonicalTypeId` delegates to it) and by cache canonization.
The size-based canonical field order and the aggregate size/alignment helpers
(`types::typeByteCount`, `types::typeAlignBytes`, `types::tagByteCount`) live
there too, so a comparator change cannot silently desync lowering from the
persisted registry. `cache::canonicalDivergenceMessage` is the single E2010
wording used by both hydration paths.

`Store` gained `dropInvalid`, which removes a bad artifact and manifest entry
after validation failure. This prevents repeated parsing/checking of the same
corrupt artifact during the session.

TypeIntern accessor gotcha: `getStructDef`/`getEnumDef`/`getUnionDef` take the
composite `TypeId`, not the internal `def_id`. Passing `structure->def_id` (the
row index) into `getStructDef` reads the wrong row and can crash. Use
`lookupStructDef(def_id)` when you only have the internal id.

ZIRL decoders now validate `u32` counts before `resize` using
`ByteReader::canReadU32Count`. The check is conservative: every element must
have room for at least four bytes in the remaining payload before the decoder
attempts the container allocation.

Cache-on-disk headers are not re-parsed by this work; that is a future debt.

`HirMakeDyn` IS serialized, despite older notes and ADR-0037 claiming it lives
in memory only. Two writers emit it: `artifact-builder.cpp` (native `.zirl`
cache, `CompactExprKind::MakeDyn`) and `wasm/abi-hir.cpp` (flat HIR blob), with
matching readers in `persistent-cache.cpp` and `wasm/abi-hir.cpp`. Any new
`HirMakeDyn` field must round-trip through both or the hydrated HIR feeds
codegen with a default. The `value_is_place` flag rides the already-serialized
`CompactExpr::flags` byte in the native cache (no version bump) and a new flag
byte in the WASM blob (`kBlobVersion` 1 to 2, since that encoding had no spare
byte and old readers would misparse).

## Verification Used For This Work

```text
cmake --build build --target test-memory test-cache zithc -j4
./build/test-memory
./build/test-cache
./build/test-zirl-sections
cmake --build build --target fmt-check
ctest --test-dir build --output-on-failure
```

`test-memory`, `test-cache`, `test-zirl-sections`, and `fmt-check` pass.
The full CTest run still reports the known `test-codegen` failures documented in
`docs/implementation-debt.md`, plus the pre-existing
`examples/ownership-advanced.zithm` IR failure. Both sets reproduce on `HEAD`
without this optimization work, so they are not regressions from these edits.

## Other Follow-Up Debts

- System C header parsing is still an outstanding cost. A session-scoped
  header-read cache was discussed, but this pass did not implement it.
- `--no-cache` and the fresh `Store::dropInvalid` path are first iteration
  cache hygiene; on-disk cache versioning/schema details remain in
  `docs/implementation-debt.md`.
- The next optimization pass can look at map-heavy HIR/SEMA loops again with a
  benchmark before and after, using `cpp_perf` where timing claims matter.
