# Stdlib C File Surface (c/fs, c/posix)

Raw file access ships under `c/`, not `std/fs`. `stdlib/c/fs.zithm` is the
portable C stdio surface (`FILE`, `fopen`/`fclose`/`fread`/`fwrite`/`fseek`/
`ftell`/`fflush`/`feof`/`ferror`/`remove`/`rename`, plus `SEEK_*`).
`stdlib/c/posix.linux.zithm` is the Linux POSIX file and `mmap` surface. The
design is recorded in `docs/adr/0046-c-file-surface-and-deferred-safe-std-fs.md`.

## Why c/ and not std/fs

The `std/fs` sketch in `docs/20-standard-library.md` used `T!`, which is not in
the Zith-- type surface, so it was stale. The safe wrapper (owning `File`,
`init`/`destroy`, named error enum) stays deferred behind three live blockers,
not behind age: allocator ownership standardization, the `DynArray`/`Result`/
`panic` foundation, and mutable `lend`/`dyn` receivers for `readInto`-style
APIs. `c/` is the raw FFI layer, so raw file access belongs there and unblocks
the bootstrap driver now.

## Platform variant, not a portable declaration

ADR-0028 requires every `c/...` declaration to have a backend implementation or
an explicit error path for every target. `mmap` and the POSIX syscalls do not
exist on WASM, so they ship only as a `.<os>` platform variant:
`c/posix.linux.zithm`. `import c/posix` resolves on Linux and reports
`E1006 missing generic module or matching platform variant` elsewhere. The
variant mechanism is documented in `memory/platform-imports.md`.

## Facts and gotchas

- `raw opaque` is `void*`. A `mmap` return is `?raw opaque`, and `munmap`
  takes `raw opaque`. Writing `?*raw opaque` would mean `void**` and fail to
  cast to a pointer for a slice view.
- `SEEK_SET`/`SEEK_CUR`/`SEEK_END` live only in `c/fs`. Declaring them in both
  `c/fs` and `c/posix` triggers `E2002 duplicate binding` when a file imports
  both.
- The `c/` convention is declaration lines with no trailing `;`, matching
  `c/io.zithm`. `zithc fmt` would add `;`, but the folder does not use it and
  `stdlib/` is outside the CMake `fmt-check` glob.
- `stat`/`fstat` write into a caller-provided opaque buffer of at least
  `STAT_BUFFER_SIZE` bytes. Field access and directory listing are deferred
  with the `struct stat`/`dirent` layout decision.
- Coverage lives in `tests/test-stdlib-io.cpp`: portable `c/fs` checks, a
  Windows-target resolve for `c/fs`, a Linux-target resolve for `c/posix`, and
  the `E1006` missing-variant case.
