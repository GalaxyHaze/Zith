# Cache Helper

## Purpose

`src/cache` provides the persistent per-module artifact cache used by later
compiler pipeline stages. It ports the Store/Manifest architecture from the
`main` branch without depending on the HIR, type, symbol, or zirl types that
exist there. The binary format and section wiring are declared in
`cache.rules` and generated into `build/src/cache/`.

## Files

| File | Responsibility |
|---|---|
| `cache.rules` | Declares format name, magic, version, metadata fields, section order, encoding rules, serialized records, enums, and sections. |
| `generate.py` | Emits the cache file header, section table, section view helpers, and the record/section codec. |
| `cache-error.hpp` | `CacheError`, `CacheErrorKind`, and explicit result aliases. |
| `cache-buffer.*` | Fixed little-endian buffer writer/reader used by the codec. |
| `cache-codec.*` | Generic section encoding/decoding and checksum validation. |
| `cache-codec.gen.*` | Generated record and section serializers/deserializers from `cache.rules`. |
| `cache-types.hpp` | Handwritten Artifact domain types serialized into sections. |
| `cache-entry.*` | `CacheEntry` and artifact validation helpers. |
| `manifest.*` | Manifest persistence and transitive reverse-dependency invalidation. |
| `cache.*` | Thread-safe artifact `Store` with hit/miss metrics. |

## Rules Syntax

```text
[format]
name: generic-cache
magic: "zgcache1"
version: 1
endian: little

[metadata]
cacheKeyHash: uint32

[sections]
Metadata
Deps

[encoding]
length: uint32
align: 8
checksum: fnv1a64

[record.DependencyRecord]
cpp = "toolkit::cache::DependencyRecord"
canonical_path: string
import_key: string
public_abi_hi: u32
public_abi_lo: u32

[enum.DeclKind]
cpp = "toolkit::cache::DeclKind"
Fn = 0
Variable = 6

[section.Deps]
items: vector[DependencyRecord] max_items = 4096
```

Supported v1 serialized field types are `u8`, `u32`, `u64`, `i64`, `float64`,
`string`, `bool`, `enum[Name]`, records declared in the same rules file, and
`vector[T]` with `max_items`. Sections can list scalar/record fields or a
single `items: vector[Record]` payload. The generator validates duplicate
names, unknown types, duplicate enum values, absent record references, and
record reference cycles.

## Verification

```bash
cmake --build build -j
ctest --test-dir build -R cache --output-on-failure
```

## Agent Boundary

Change `cache.rules` for the format, section, record, and enum surface. Keep
header/container encoding, checksum validation, and Store/Manifest behavior in
the handwritten C++ files listed above. Do not edit `build/src/cache/*`.

## Public API

The handwritten and generated headers jointly expose:

- `Store` with `store`, `load`, `loadEntry`, `invalidate`, `manifestEntry`, `metrics`, and `root`.
- `CacheKey` and `ContentFingerprint` for identity and content validation.
- `Artifact` with domain metadata, dependency records, declaration records, and serialized payloads.
- `CacheEntry`, `ManifestEntry`, and generated `encodeArtifact`/`decodeArtifact` helpers.

`Store` writes per-module artifacts under a cache root, maintains a manifest, and tracks
hit/miss/invalid/write metrics.

## Demo

`tests/cache/cache-demo.cpp` creates a temporary `Store`, stores one `Artifact`, reloads it with a
matching fingerprint, forces a miss with a different fingerprint, and prints the resulting metrics.

```bash
cmake --build build --target cache-demo -j
ctest --test-dir build -R '^cache-demo$' --output-on-failure
```
