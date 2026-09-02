# Canonical Type IDs for Opaque Hydration

Bare `opaque` must be re-hydratable across modules and persistent cache. The tag
stored at runtime stays a project-local `u32`, but the compiler derives a stable
128-bit canonical type identity from namespace, ordered fields, and the type
name, then maps each canonical type that actually participates in `opaque` to a
runtime `u32` through a persistent project registry.

Status: accepted

Update: `canonicalTypeId` agora usa o módulo definidor de cada tipo nomeado em vez do
módulo onde o lowering decorre. Structs, enums, unions, aliases nominais e reificações
genéricas propagam esse owner a partir da `TypeTable` partilhada do sema, tornando
`at-canonicalType(T)` estável entre módulos e sessões.

Considered Options:

- Use the 128-bit canonical id directly in the opaque runtime tag. That would
  make the tag globally unique and collision-free, but changes the tagged union
  representation and ABI.
- Assign runtime ids by deterministic hashing of the canonical id. That avoids a
  registry, but sacrifices project-local dense ids and cannot give stable
  sequential ids across incremental builds.
- Keep only a module-local `u32` hash. That is the current behavior, but it
  cannot hydrate cached or imported opaque values because the tag has no stable
  definition identity.
- Store the canonical id only on each compact type. That would serialize more
  data than needed; the chosen design records canonical mappings only for types
  that were actually erased into opaque.

Consequences:

- A `TypeCanonicalId { uint64_t hi, lo; }` becomes the stable in-memory and
  serialized identity used by opaque hydration.
- Canonical identities are lazy: they are computed only when a type needs one
  (opaque payload, cache mapping, or semantic comparison that requires
  canonical identity).
- The runtime tag remains `{ *void, u32 }`; the compiler guarantees that one
  project assigns one `u32` per canonical type.
- A new root cache file named `canonical-any` records canonical ids that have
  received a runtime id for the project. The runtime id of an existing
  canonical id is stable; newly discovered canonical ids are appended after the
  persisted set in a deterministic order only when they are actually used as
  opaque payloads.
- Each artifact serializes its own `canonicalMappings` vector with
  `{ canonical_id, runtime_id }`. Hydrated artifacts trust the mapping stored in
  the artifact as long as the canonical id still exists in the root registry.
- ZIRL format version is bumped to 15 when this lands; old caches become misses
  and are regenerated.
- `TypeStruct`/other named composite definitions need enough owner/module state
  to compute their canonical id from the defining module, not from the consumer
  module. The same applies to enum, union, alias, nominal, and generic
  instantiation identities.
