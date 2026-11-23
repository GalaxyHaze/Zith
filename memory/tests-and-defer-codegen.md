# Tests, Build Roots, and Defer Codegen

The focused test executables are registered by CTest and compiled into the
build root, not into `build/tests/`. Running `./build/test-frontend`,
`./build/test-sema`, `./build/test-hir-lower-modern`,
`./build/test-formatter`, or `./build/test-codegen` after
`cmake --build build -j` works from the repository root.

## Defer Coverage and Semantics

`defer expr;` and `defer { ... }` are implemented through frontend, sema, HIR,
and LLVM codegen. Dedicated coverage lives in the existing focused suites:
`tests/test-frontend.cpp`, `tests/test-sema.cpp`,
`tests/test-hir-lower-modern.cpp`, `tests/test-formatter.cpp`, and
`tests/test-codegen.cpp`.

Cleanup registration is scoped to the nearest lexical block, runs in reverse
registration order before that block's terminator, and also runs before
`HirStateTailCall` in `state` bodies. A `defer { ... }` body is cleanup-only,
does not produce a block value, and rejects `return`, `break`, `continue`, and
`jump`.

`tests/test-macro.cpp` covers variadic kind validation (`val`, `identifier`,
and `ast`), unsupported kinds, and missing fixed arguments.
`tests/test-codegen.cpp` verifies ordered repetition, empty tails, caller-block
cleanup on fallthrough, return, and loop break, and LIFO ordering between
separate defer registrations.

In this sandbox, LeakSanitizer can terminate before tests with a fatal
ptrace-related error. Running an ASan/UBSan test binary with
`ASAN_OPTIONS=detect_leaks=0` works, but does not verify leaks.

## Current Verification

`cmake --build build -j`, `ctest --test-dir build --output-on-failure`, and
`./build/zithc --include stdlib check build/main.zithm` pass with the
`defer`/void-`state` work in the tree. The modern file type-alias codegen test
also passes after the current parser/sema state in this worktree.

On 2026-11-23, the focused `test-macro` and `test-codegen` CTest cases passed
for variadic macro repetition and caller-block cleanup.
