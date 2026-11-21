# Standard Library `DynArray`, `Result`, and `panic`

Status: drawing

This drawing adds the essential allocator-backed `DynArray<T>` and a small
error-handling foundation to the standard library. `DynArray` is the primary
deliverable. Its constructors take an explicit allocator, `reserve` and `push`
report failure without changing the prior state, and access/removal return an
optional value. `cleanElements` explicitly cleans `InPlace` elements, clears
the length, and keeps capacity; call it before `destroy` when elements own
resources. `destroy` frees only the backing buffer.

The same expansion adds a single positional generic `Result<T, E>` union
(`union Result<T, E> { T, E }`, the canonical form) with generic `Ok`/`Err`
helpers, contextual inference, `Result<void, E>`, `isOk`/`isErr`, and
`unwrap`/`unwrapErr`. Unwrapping the wrong case panics. `E` is unrestricted,
payload formatting is not part of panic, and payload cleanup remains the
caller's responsibility. No `?` integration is included. A public fatal
`panic(message)` reports through the backend's diagnostic channel and does not
recover.

Size: module, the standard-library collections and error-handling surface.

Non-goals:

- Allocator standardization is tracked separately in
  [allocator ownership](allocator-ownership.md), and must land first.
- `new`, `delete`, `make`, and `release` remain outside this work.
- `std/fs`, automatic destruction, and `?` integration remain deferred.
- No language-wide ownership or error-propagation feature is added.

Open decisions:

- Validate `DynArray` layout, growth overflow, alignment, and failure behavior
  against the allocator contract.
- Probe the compiler support needed for contextual inference and
  `Result<void, E>`. Named union cases are not part of the language; `Ok`/`Err`
  are plain generic helpers over the positional union. Generic union helper
  monomorphization now keeps concrete type arguments (see
  `test-enum-union-generics`), so the earlier `Ok`/`Err` blocker is cleared.
  Contextual (expected-result) inference now lands in
  `GenericInstantiationPass`: a binding annotation or `return` unifies the
  declared result against the expected type, so `Ok(7)`/`Err(e)` bind their
  result-only parameters without explicit `<...>`. The shipped surface is
  `stdlib/std/result.zithm`. `Result<void, E>` remains open.
- Resolved: `panic(message)` ships as `stdlib/std/panic.zithm`. It writes the
  message and a trailing newline one byte at a time through `putchar`, then
  terminates through the runtime panic code `R10004` (`@assert(false)` outside a
  test). The native `__zith_runtime_panic` helper now flushes the C streams
  (`fflush(nullptr)`) before `llvm.trap`, so the message survives the abort even
  when the program's output is a pipe. The VM v2 lowering accepts any `R10001+`
  code (the generic `Op::Panic` carries it), so the same source runs on both
  backends. `test-codegen` (`test_imported_panic_runtime`) and
  `test-stdlib-modules` cover it.
