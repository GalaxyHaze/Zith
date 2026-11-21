# C File Surface and Deferred Safe `std/fs`

Status: accepted for the Zith-- iteration.

The compiler and any future bootstrap driver need to open, read, and write
files. The shipped `c/` surface has console, allocation, and string
declarations but no file access, and the `std/fs` sketch in
[20-standard-library.md](../20-standard-library.md) is stale: it uses `T!`,
which is not part of the Zith-- type surface. This records where the raw file
bindings live now and why the safe wrapper stays deferred.

## Raw file bindings live in `c/`, the safe wrapper stays deferred

The `c/` namespace is the raw FFI layer, not a safe wrapper
([CONTEXT.md](../../CONTEXT.md)). File access starts there, the same way
`c/io`, `c/stdlib`, and `c/string` already do, because the bootstrap driver
needs it now and the safe API does not.

Two modules ship:

- `stdlib/c/fs.zithm` is the portable C stdio file surface: `FILE`, `fopen`,
  `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `fflush`, `feof`, `ferror`,
  `remove`, `rename`, and the `SEEK_*` constants. It resolves on every
  supported target through libc, a VM intrinsic, or a WASM host import.
- `stdlib/c/posix.linux.zithm` is the Linux POSIX file and memory-mapping
  surface: `open`, `close`, `read`, `write`, `lseek`, `fstat`, `stat`,
  `access`, `unlink`, `mmap`, `munmap`, and the `O_*`, `PROT_*`, `MAP_*`, and
  access-mode constants.

The safe `std/fs` wrapper, with an owning `File`, `init`/`destroy`, and a
named error enum, remains deferred. It is blocked behind three open items, not
behind the age of the sketch: the allocator ownership standardization
([allocator-ownership.md](../plans/stdlib/allocator-ownership.md)), the
`DynArray`/`Result`/`panic` foundation
([dynarray-result-panic.md](../plans/stdlib/dynarray-result-panic.md)), and
mutable `lend`/`dyn` receivers for `readInto`-style APIs
([ADR-0022](0022-stdlib-io-format-split.md)).

## `mmap` is a platform variant, not a portable declaration

ADR-0028 requires every declaration added to `c/...` to have a backend
implementation or an explicit capability or error path for every supported
target. `mmap` and the POSIX syscalls are POSIX and Win32, and `mmap` does not
exist on WASM. They therefore do not belong in a portable `c/` module.

The module resolver already provides the mechanism: a platform variant such as
`foo.<arch>.<os>.zithm` is preferred over the generic `foo.zithm`
([platform-imports.md](../../memory/platform-imports.md)). `c/posix.linux.zithm`
is the `.<os>` variant, so `import c/posix` resolves only on Linux. On a target
without a matching variant the import fails with the resolver's
missing-platform-variant diagnostic, which is the explicit error path ADR-0028
asks for. Windows and other hosts add their own `c/posix.<os>.zithm` variant
when they are supported.

`c/fs` stays portable and has no platform variant, so it never shadows a
platform-specific surface. Callers that need `mmap` import `c/posix` directly.

## Consequences

- The stale `std/fs` sketch in `20-standard-library.md` is marked deferred and
  points at `c/fs` and `c/posix` instead of the `T!` signatures.
- `mmap` returns a raw mapping handle and its length. Typed views over the
  mapping, page-alignment rules, and the lifetime that ties the mapping to its
  file descriptor belong to the deferred safe wrapper, not to `c/posix`.
- Directory listing is out of scope for this iteration. `opendir`/`readdir`
  need a `dirent` layout decision that the validated-C rules do not cover yet,
  so `c/posix` ships `access` and an opaque-buffer `stat` for existence and
  metadata, and directory enumeration is a follow-up.
- `stat` and `fstat` write into a caller-provided opaque buffer. Field access
  is deferred with the layout decision above.
- The bootstrap driver can read sources, read `.zirl` artifacts, write
  artifacts, and map large files through these two modules without waiting for
  `std/fs`.
