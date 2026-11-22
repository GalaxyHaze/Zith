# WASM VM v2 Execution ABI

Status: accepted

## Context

The WASM playground must provide the same language and standard-library
surface as the native portable runtime. The current browser artifact exposes
compile and HIR exports, but the website still treats `zith_run_source` as a
compile-only operation, and importing the virtual `std/io/console` module can
trap with `memory access out of bounds` before execution.

The older execution IR (`exec-ir.hpp`, `hir-to-ir.*`, `ir-vm.*`) is archived
under `archive/execution-ir-v1/` and is not in the active tree; VM v2 is the
default portable runtime.

## Decision

The WASM playground gains two execution exports.

`zith_emit_hir(ptr, len)` compiles a source buffer through the modern pipeline
to `HirLowered`, serializes the lowered module into a versioned, stateless
flat HIR blob, and makes the blob available through
`zith_last_buffer_ptr`/`zith_last_buffer_len`. The blob is self-contained:
it carries strings, types, functions, and expressions and can be
reconstructed without the original source or compiler session.

`zith_execute_hir(ptr, len)` decodes a flat HIR blob, lowers it into VM v2 IR,
and runs it through the VM. The VM output is written through the existing
`zith.host_write` import and the exit code is exposed through
`zith_exit_code`.

`zith_run_source(ptr, len)` is the high-level browser operation. It compiles
the source, executes the resulting HIR through VM v2, forwards stdout/stderr,
and exposes the program exit code. `zith_emit_hir` and `zith_execute_hir`
remain separate so a later cache can compile once and replay a compatible
HIR blob.

The browser does not maintain a hand-written fork of the standard library.
The canonical `stdlib/` tree is packaged by the release workflow into a
versioned stdlib distribution artifact with ABI/version/hash metadata. The
website update step publishes the matching WASM artifact and stdlib pack
together. The WASM runtime receives the pack through an explicit host/ABI
boundary; it does not assume that browser WASM can search an HTTP directory.

The public status codes are:

```text
0 ok
1 compile fail
2 invalid parameter
3 runtime trap
4 out of memory
5 unsupported construct
```

The flat HIR format lives under `src/wasm/abi-hir.*`. It is independent of
`.zirl`/cache even though it uses the same version-first, string/type/function
structure so a later artifact cache can adopt or extend it without breaking
the JS ABI.

The flat HIR format is version 7. Section marker `6` introduces module-local
vtables with ordered function slots. Marker `7` introduces concrete struct
definitions, including ordered field metadata, type arguments, and layout data.
VM v2 uses this information to execute serialized `HirMakeDyn` and `HirDynCall`
expressions after a round trip. The tested `dyn` support boundary is this VM
v2/WASM dispatch path. It does not imply support in every compiler backend.

The WASM execution ABI remains version 2. The flat HIR version is a separate
format version, and the decoder rejects blobs from another flat HIR version.

The old execution IR v1 implementation is archived under
`archive/execution-ir-v1/` and removed from active builds and tests. It is
kept in the repository for historical reference, but not compiled.

`--interpreted` stays exclusively on the HIR interpreter. The no-LLVM and
WASM runtime path uses VM v2.

## Consequences

- JS clients can compile once, store the flat HIR blob, and execute later or
  reuse it across page sessions without recompiling source.
- `zith_run_source` is an executing convenience operation, while the explicit
  HIR exports remain available for cache integration.
- The first slice supports the same VM v2 subset as the host harness: string
  literals, arithmetic, slots, direct calls, `stdio.console.println`, and a
  few extern handlers. Unsupported constructs return `5`.
- The host VM v2 tests remain the lowering contract; the WASM harness is a
  smoke test that the flat HIR round-trips and the same VM runs it.
- Compiler diagnostics, unsupported HIR, runtime traps, and invalid ABI
  parameters remain distinct status classes. User source errors must never
  surface as an uncaught WASM trap.
- The playground implements the compile-once artifact cache the ABI promised:
  `zith_compile_hir` stores a flat HIR blob in a module-local cache,
  `zith_execute_cached` replays it without recompiling, and `zith_restore_cached`
  loads a host-persisted blob. Cache misses and stale blobs are reported through
  the structured diagnostics channel instead of triggering a silent recompile.
  The host still owns durable storage across page sessions.
- The old execution IR plan and ADR are historical references, not active
  contracts.
