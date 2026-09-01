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
- `NraFacts::narrowing_facts_` is copyable but not pointer-stable after
  insertion/rehash. Code that needs both old and new facts copies the values
  before writing through the map.
- `ModuleCache::invalidateLocked` first collects eviction keys, then erases, so
  it does not invalidate iterators.

## Session Cache State

The CLI now exposes `--no-cache` to skip persistent `.zith-cache` reads and
writes for an invocation. In-memory frontend memoization still applies within
the session.

`Store` gained `dropInvalid`, which removes a bad artifact and manifest entry
after validation failure. This prevents repeated parsing/checking of the same
corrupt artifact during the session.

ZIRL decoders now validate `u32` counts before `resize` using
`ByteReader::canReadU32Count`. The check is conservative: every element must
have room for at least four bytes in the remaining payload before the decoder
attempts the container allocation.

Cache-on-disk headers are not re-parsed by this work; that is a future debt.

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
`examples/ownership-advanced.zith` IR failure. Both sets reproduce on `HEAD`
without this optimization work, so they are not regressions from these edits.

## Other Follow-Up Debts

- System C header parsing is still an outstanding cost. A session-scoped
  header-read cache was discussed, but this pass did not implement it.
- `--no-cache` and the fresh `Store::dropInvalid` path are first iteration
  cache hygiene; on-disk cache versioning/schema details remain in
  `docs/implementation-debt.md`.
- The next optimization pass can look at map-heavy HIR/SEMA loops again with a
  benchmark before and after, using `cpp_perf` where timing claims matter.
