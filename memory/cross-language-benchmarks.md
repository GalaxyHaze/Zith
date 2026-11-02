# Cross-Language Benchmark Memory

The standalone runtime baseline lives in `benchmarks/cross-language/`. It
compares matched Zith--, C, and Rust kernels, validates full checksums, excludes
compilation from timing, and includes process startup in wall-clock samples.

## Implementation Pitfalls

- `ZITH_BUILD_BENCHMARKS=ON` currently stops in `CMakeLists.txt` because the
  archived lexer benchmark sources are gone. Keep the cross-language driver
  standalone unless that CMake path is restored.
- `rustc` can be a rustup proxy symlink. Preserve the `rustc` executable name
  when making its path absolute. Resolving the symlink changes `argv[0]` to
  `rustup`, which invokes the toolchain manager instead of the compiler.
- `state` is a reserved Zith-- keyword. Use another local binding name, such
  as `rng`.
- `sequenta orient --module` expects the full task identifier, such as
  `cross-language::suite`, rather than only the module name.
