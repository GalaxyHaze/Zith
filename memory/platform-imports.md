# Platform-Specific Imports

## Summary

Implemented: `import foo` resolves platform variants such as
`foo.<arch>.<os>.zithm` against the active compilation target while keeping the
generic `foo.zithm` as a required fallback. The feature belongs in the module
resolver, not in comptime, macros, or a new conditional statement.

Full design archive: `docs/plans/archive/platform-imports.old.md`. The locked
contract (resolution order `arch.os` -> `arch` -> `os` -> generic, last-segment
variants, no platform directories, no v1 ambiguity error, vendor/environment
excluded) lives there and is not restated here.

Active status is `Working` in `docs/impl-status.md` and F-42 in
`docs/roadmap.md`. This note supersedes the older "planned" wording; the
implementation contract and tests moved to the archived plan.

## Current Ground Truth

`FrontendContext::resolveImport()` checks a regular file at the requested path,
platform suffix variants, the generic `path.zithm`, a same-named directory, and
`path/mod.zithm` per search root. The directory is a fallback, so a facade file
such as `std/memory.zithm` wins over `stdlib/std/memory/`.
`--target` reaches `FrontendConfig::targetTriple` and participates in
`CacheKey::identity()`, so the module cache remains separated per target.

The resolver code moved with the frontend monolith split. The current home is
`src/session/frontend-module-analysis.cpp`; do not point future edits at the
old pre-split `frontend-context.cpp` implementation sketch.

## Durable Facts

- Canonical `llvm::Triple` names are used, with no aliases such as
  `amd64`/`arm64`.
- Cache stays target-separated for now; sharing generic module artifacts across
  targets is documented future work.
- Regular-file candidates win before a same-named directory is aggregated.
- Missing Zith imports mention both the generic module and the platform variants
  when platform candidates were considered.
- `export foo` re-exports the resolved platform file for the current target.

## Gotchas

- Parse happens before target-based resolution, so only the resolver changes;
  the frontend parsing path stays untouched.
- Without LLVM, target component extraction returns no components and no
  platform variants are generated; code is compiled without leaking
  `llvm::Triple`.
- C header imports are handled by a different branch and must not start matching
  Zith platform variants.
- The persistent/object cache paths already use target keys, so no new top-level
  cache layout is needed.
- The resolver uses `fs::is_regular_file` for literal, variant, and generic
  files. It aggregates a same-named directory only when those files do not
  match, then checks `foo/mod.zithm`.
- Only the last path segment varies for platform-specific imports.
