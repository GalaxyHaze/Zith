# Project Memory

Durable, topic-focused notes about the Zith codebase and development workflow.
This complements `docs/` and the code search indexes by recording the non-obvious
contracts, decisions, and gotchas that are not discoverable from the source alone.

## Conventions

- Create one file per topic or area under `memory/`.
- Keep each file as short as useful. Operational notes do not need 200 lines
  unless they carry a real playbook; long design narratives belong in `docs/`.
- Prefer one source of truth per contract: when `docs/` already owns the
  contract, `memory/` records only the non-obvious implementation facts and
  links to `docs/`.
- Read the relevant file before working on that area.
- Update notes when the repository behavior changes.
- Invalidate or rewrite entries that are outdated instead of leaving
  contradictory notes.

## Index

- [audit-cleaning.md](audit-cleaning.md): current repo-hygiene audit state,
  deferred WIP, and the cleaning queue.
- [comptime-generics-traits.md](comptime-generics-traits.md): full-Zith 0.7.0
  comptime/traits/capability context; details live in
  `docs/plans/archive/0.7.0-zith/` and this note now points there.
- [monolith-splits.md](monolith-splits.md): short pointer to the completed
  frontend/session split decisions. See `docs/plans/monolith-splits.md` for the
  execution contract and line counts.
- [sra-hir-boundary.md](sra-hir-boundary.md): stable ownership/HIR boundary and
  the pitfalls that keep the pre-HIR SRA contract intact.
- [nra-design.md](nra-design.md): future `Zith` NRA design decisions and link to
  the full `docs/nra-spec.md` contract.
- [platform-imports.md](platform-imports.md): implemented `Zith--` platform
  variants for `foo.<arch>.<os>.zith` plus cache/target decisions.
- [build-c-compile.md](build-c-compile.md): companion `.c` discovery/link flow,
  current backend selection rules, and the Clang `-Weverything` gotchas hit
  while landing the feature.
- [simd-intrinsics-asm.md](simd-intrinsics-asm.md): decision to prefer LLVM
  intrinsics over assembly for native/SIMD support; vector types come after
  scalar intrinsics. Also records the 2026-09-14 full-Zith design for machine
  intrinsics, `unsafe asm`, and platform deltas, plus the known `@sizeOf`
  `E5001` lowering gap.
- [stalin-debug.md](stalin-debug.md): strategy for narrowing compiler bugs by
  disabling reproducer/code paths step by step before deep debugging.
- [parser-depth-limit.md](parser-depth-limit.md): recursive parser depth cap,
  iterative recovery contract, post-audit test results, and local sanitizer
  validation caveats.
- [packages-lockfile.md](packages-lockfile.md): lockfile path identity gotchas
  and the CMake reconfiguration requirement for new package sources.
- [sanitizer-ci.md](sanitizer-ci.md): ASan/UBSan workflow contract, local
  and successful remote sanitizer verification, cross-build failure
  attribution, and the recorded unprotected `main` branch state.
- [session-arena-lifetime.md](session-arena-lifetime.md): `CompilationSession`
  member lifetime dependency between `mScratchArena` and `mProjectConfig`.
- [tests-and-defer-codegen.md](tests-and-defer-codegen.md): test binary
  locations, `defer` codegen notes, and the known modern-file alias codegen
  failure outside the defer work.
- [plan-debt-status.md](plan-debt-status.md): plan/debt/status curation
  contract, the current feature status map, and the docs audit method.
- [agent-scheduler.md](agent-scheduler.md): scheduler/agent-wait contract,
  `.awt/agent-state` writer rules, merge/test flow, and Sequenta bridge status.
- [flat-containers-cache.md](flat-containers-cache.md): FlatMap/FlatSet API
  contracts, hot-map migrations, cache/CLI `--no-cache`, and validation checks
  landed during the consolidation pass.
- [hir-lowering-call-targets.md](hir-lowering-call-targets.md): distinguish a
  resolved call target from receiver-call syntax when lowering explicit `self`
  parameters.
- [release-install-layout.md](release-install-layout.md): stdlib discovery
  contract and installer fixes. Release audit snapshots live in
  `docs/plans/archive/release-*.old.md`.
- [branch-protocol-draft.md](branch-protocol-draft.md): short pointer to the
  full-Zith thread-model draft in `docs/plans/branch-protocol.md`.
- [discord-mcp.md](discord-mcp.md): ByteAsk Discord MCP server registration,
  the stdio-only limitation of the v1.0.0 release JAR, and the env-var/token
  requirements to run it.
- [idea-lifecycle.md](idea-lifecycle.md): file-backed idea lifecycle, ABI
  state map, and the struct-layout unit used for conforming contracts.
- [execution-ir-drawing.md](execution-ir-drawing.md): status of the archived
  execution IR v1 and a pointer to the VM v2 contract.
- [vm-v2-drawing.md](vm-v2-drawing.md): VM v2 boundaries and the typed
  register semantics required for floating-point execution.
- [stdlib-io-format.md](stdlib-io-format.md): proposed `std/io/consoleIn`/
  `consoleOut` format design, `TextSink`/`dyn` probe facts, and target access
  pattern for variadic `Formatable` values.
- [stdlib-io-result.md](stdlib-io-result.md): proposed `Result<T, E>` surface
  and the compiler facts behind generic `Ok`/`Err` helpers; now shipped as
  `stdlib/std/result.zith`, plus the shipped fatal `panic(message)`
  (`stdlib/std/panic.zith`, `R10004`) and why `Result<void, E>` stays open.
- [comptime-contextual-inference.md](comptime-contextual-inference.md):
  expected-result generic inference (weak literal vs strong expected-result
  binding) and where it lives, enabling `Ok(7)`/`new(3)` without `<...>`.
- [stdlib-c-fs.md](stdlib-c-fs.md): raw file access under `c/fs` and the
  Linux-only `c/posix` platform variant, with the `raw opaque` and
  duplicate-`SEEK_*` gotchas and the deferred safe `std/fs` blockers.
- [stdlib-soon-modules.md](stdlib-soon-modules.md): the `soon/` modules, the
  `soon/result/fallback`, `soon/math/scalar`, and
  `soon/algorithms/slice` experiments, the
  `test-stdlib-modules` guard that type-checks every `stdlib/**.zith`, and the
  `return expr;` / narrowing gotchas that broke `soon/string.zith`.
- [stdlib-imported-generics.md](stdlib-imported-generics.md): imported generic
  structs (`HashMap<K,V>`, `DynArray<T>`) are now consumable from another
  module; the qualified struct-literal, imported field-default, transitive
  concretization, and optional-coercion fixes behind ADR 0020.
- [sema-nonnull-narrowing.md](sema-nonnull-narrowing.md): how `?*T` non-null
  proofs are stored/consumed, why a write to the binding invalidates them
  (soundness fix), the optional-aggregate (`?S`/`?pack`/`?i32`) narrowing
  extension, aggregate literals coerced to `opaque`, the resolved `null`
  pack-member inference (`null as ?T` and the annotation path), and the
  remaining local-only narrowing debt.
- [sema-generic-union-void.md](sema-generic-union-void.md): a generic union
  instantiated with `void`/`never` (for example `Pair<void>`, `Result<void, E>`)
  is rejected with `E3001` at the use site instead of hanging LLVM codegen in
  `getTypeAllocSize`; the fix mirrors the declaration-path rule and
  `Result<void, E>` support stays a separate, larger item.
- [lsp-vscode-folding.md](lsp-vscode-folding.md): LSP/VS Code brace folding
  flow, local VSIX rebuild loop, and read-only sandbox gotchas.
- [test-command-coordination.md](test-command-coordination.md): how the native
  `zithc test` work is split between the master and the hermes worktrees, the
  file-ownership boundary, and the verification gates.
- [test-stdlib-check-macros.md](test-stdlib-check-macros.md): macro expansion
  scope and VM formatting constraints for accumulated std/test diagnostics.
- [wasm-strict-warnings.md](wasm-strict-warnings.md): why the WebAssembly build
  rejects namespace-scope `std::string` constants and `constexpr std::string`,
  and the `constexpr std::string_view` plus accessor pattern that works.
