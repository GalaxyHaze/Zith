# Zith-Owned C Binder and Tiny Backend Roadmap

Zith will progressively reduce its dependence on the LLVM project. For C interop, the immediate plan is a Zith-owned C binder embedded in the compiler that parses headers into the existing `cinterop` artifact without libclang. The later C backend for companion `*.c` files will be a lightweight Zith-owned compiler ("tiny C backend") focused on small size and stable simple C, not optimization.

Status: proposed

Considered Options:

- Keep libclang as the only header importer. It is the most complete parser available, but it ties header imports to a large external dependency and blocks standalone/WASM workflows in practice.
- Use TinyCC as the long-term embedded C compiler. It is small but still an external project, and its portability/ABI validation does not match the targets Zith wants to own.
- Adopt Clang as a temporary bundled C driver while building Zith-owned C parsing/emission behind the same interface. This keeps development unblocked and creates a clean replacement point.

Consequences:

- A `CHeaderParser`/toolchain abstraction must exist so libclang and the Zith-owned binder can both produce `CHeaderArtifact`.
- Header import moves to "validated surface only": unsupported declarations are rejected with clear diagnostics rather than silently imported with unverified ABI.
- Companion C compilation can keep a temporary bundled Clang backend while the tiny C backend matures; the public toolchain API does not change when the backend is swapped.
- WASM receives a small header bundle from Zith so browser builds do not need host C headers.
