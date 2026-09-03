# Tiny C Backend Plan

Goal: build a small Zith-owned C object emitter for companion `*.c` sources and future standalone C support, focusing on small footprint, stable behavior, and correct simple C17 instead of optimization.

Status: planned, not started.

## Target Scope

First supported native targets:

- ELF x86-64 (`x86_64-unknown-linux-gnu`).
- ELF AArch64 (`aarch64-unknown-linux-gnu`).

First accepted source surface:

- Functions with scalar, pointer, and validated simple-record ABI.
- `extern`, `static`, declarations, function calls, control flow, arithmetic, comparisons, assignments, and C17 scalar initializers.
- C header bindings that the current binder already validates.

Rejected until separately validated:

- Variable-length arrays.
- Bitfields.
- `goto`.
- Complex macros.
- Flexible array members.
- Packed structs.
- Unverified struct-by-value ABI.
- Cross-target host compilation outside the validated ELF targets.

## Design Constraints

- The backend must run inside `zithc` without libclang, TinyCC, or LLVM as a dependency.
- Object emission must be deterministic for the same source, target, and compiler version.
- Output must be a standard native object file that the embedded LLD can link.
- Error messages must be user-actionable and consistent with existing Zith diagnostics.
- Optimization is explicitly out of scope; the backend may emit simple, straightforward code.

## Success Criteria

- A `*.c` companion from `ffi.c_source_dirs` can be compiled end-to-end on ELF x86-64 and AArch64 without a host C compiler.
- The resulting binary passes native CTest ABI tests.
- `zithc` with the tiny C backend shows `C compile backend: zith-cc` in `zithc info`.
- Unsupported C constructs are rejected during compilation with a clear diagnostic.

## Relationship To Other Work

- The `/home/diogo/Zith/docs/adr/0008-zith-owned-c-binder-and-tiny-backend-roadmap.md` ADR records why the project is moving away from TinyCC/libclang for the long term.
- The binder work in `/home/diogo/Zith/docs/plans/standalone-c-toolchain.md` must land first so the C ABI surface is validated before object emission.
- The backend becomes the eventual replacement for the temporary bundled Clang C driver, not a parallel user-facing compiler.
