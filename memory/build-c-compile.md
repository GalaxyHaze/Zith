# C Compile Integration

The optional companion-C pipeline lives in `src/cc/` and is exercised through
`ffi.c_source_dirs` in `ZithProject.toml` plus the repeatable
`--c-source-dir <DIR>` CLI flag. `CompilationSession::prepareNativeLinkInputs()`
discovers `*.c` recursively, emits companion objects under
`cache/c-obj/<target>/`, and appends them to the native link inputs after Zith's
main LLVM object is ready.

## Current backend contract

- `libclang` remains header-import only. It is not part of `.c` compilation.
- `ZITH_ENABLE_C_COMPILE` gates the feature as a whole.
- `ZITH_C_COMPILE_AVAILABLE` means `libtcc` was found at configure time.
- `ZITH_TCC_FETCH` (default ON) fetches TinyCC 0.9.27 when no system `libtcc` exists.
- When `libtcc` is available, `cc::compileCSource()` uses it and refuses
  incompatible target triples instead of silently compiling for the host.
- When `libtcc` is unavailable but `ZITH_ENABLE_C_COMPILE` is on, the runtime
  falls back to the configured C compiler driver (`ZITH_C_COMPILER_PATH`) for
  `-c` compilation so project-local `.c` sources still build in this workspace.
- Native final linking still falls back to the compiler driver unless embedded
  LLD is actually available in the build.

## Verified gotcha

This repository builds with Clang `-Weverything -Werror`. New helper modules that
introduce fixed-width enum underlying types must include `<cstdint>` explicitly;
the surrounding headers are not enough. Clang also treats some "copy not elided"
NRVO cases as hard errors here, so short error-path returns in small aggregate
helpers are safer when constructed directly instead of returning a mutable local.
