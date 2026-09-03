# Standalone Native Toolchain Bundle

The native `zithc` distribution must let the end user install only the compiler artifact and still build Zith programs plus companion C sources. We will package LLVM, LLD, and a temporary C compiler into the artifact, while resolving the platform libc/`cstdlib` sysroot from the operating system for now.

Status: proposed

Considered Options:

- Require an externally installed LLVM/Clang/LLD. This keeps the artifact small but conflicts with the standalone goal and makes build/install instructions host-specific.
- Bundle a full sysroot/libc. This is the most hermetic option, but it is a large distribution, ABI-sensitive, and too early before the Zith-owned C path is stable.
- Bundle only LLVM/LLD plus a temporary C driver, leaving libc to the OS. This gives standalone toolchain behavior for the current pipelines without designing the final sysroot model prematurely.

Consequences:

- The distribution artifact includes LLVM libraries and LLD, so the size and license/reproducibility implications must be reviewed before release.
- `zithc` must locate bundled resources relative to itself, not through ambient `LLVM_DIR`/`PATH` values.
- The OS libc remains an explicit deployment assumption; WASM instead uses the small Zith WASM header bundle.
- The temporary C driver can be replaced by the Zith-owned C backend without changing the public toolchain contract.
