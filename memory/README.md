# Project Memory

Durable, topic-focused notes about the Zith codebase and development workflow.
This complements `docs/` and the code search indexes by recording the non-obvious
contracts, decisions, and gotchas that are not discoverable from the source alone.

## Conventions

- Create one file per topic or area under `memory/`.
- Keep each file within 300 lines, preferably 200-300 lines.
- Split overgrown topics into more focused files and link them from here.
- Read the relevant file before working on that area.
- Update notes when the repository behavior changes.
- Invalidate or rewrite entries that are outdated instead of leaving
  contradictory notes.

## Index

- [comptime-generics-traits.md](comptime-generics-traits.md): 0.7.0 planning
  contract, pipeline order, step files, diagnostic reservation, and extension
  recipe for the capability base.
- [nra-hir-boundary.md](nra-hir-boundary.md): stable ownership/HIR boundary and
  the pitfalls that keep the pre-HIR NRA contract intact.
- [build-c-compile.md](build-c-compile.md): companion `.c` discovery/link flow,
  current backend selection rules, and the Clang `-Weverything` gotchas hit
  while landing the feature.
- [simd-intrinsics-asm.md](simd-intrinsics-asm.md): decision to prefer LLVM
  intrinsics over assembly for native/SIMD support; vector types come after
  scalar intrinsics. Also records the known `@sizeOf` `E5001` lowering gap.
- [stalin-debug.md](stalin-debug.md): strategy for narrowing compiler bugs by
  disabling reproducer/code paths step by step before deep debugging.
- [tests-and-defer-codegen.md](tests-and-defer-codegen.md): test binary
  locations, `defer` codegen notes, and the known modern-file alias codegen
  failure outside the defer work.
- [flat-containers-cache.md](flat-containers-cache.md): FlatMap/FlatSet API
  contracts, hot-map migrations, cache/CLI `--no-cache`, and validation checks
  landed during the consolidation pass.
