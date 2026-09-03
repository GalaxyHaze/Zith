# Embedded LLD In-Process

Native linking will use `lld` linked into `zithcLib` and driven through its in-process API instead of executing the user's `ld.lld` or system linker. This keeps the linker inside the standalone compiler artifact and removes a host toolchain dependency from the link step.

Status: proposed

Considered Options:

- Invoke `ld.lld` as an external process. This is simpler to implement on ELF today, but reintroduces a host-toolchain requirement and makes Windows/macOS paths less uniform.
- Keep the external C driver for linking. It is already a fallback, but it directly violates the standalone distribution goal.
- Call the LLD API in-process. This reuses the existing `lld::lldMain` wiring in `src/cc/driver.cpp` and gives one embedded linker for the native profiles.

Consequences:

- The embedded LLD path must be validated for ELF first, then COFF (`Windows`) and Mach-O (`macOS`) as native profiles are added.
- `zithc` must bundle the LLVM/LLD runtime required by the embedded API.
- The external driver remains a diagnostic fallback or an explicit alternative toolchain mode, not the default native path.
- Link invocations from other in-process LLVM consumers need lifecycle/teardown care because both use the same LLVM runtime.
