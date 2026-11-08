# Stdlib I/O Result Contract And Generic Union Limits

This note records the `Result<T, E>` surface for stdlib I/O and the compiler
facts behind convenient generic `Ok`/`Err` helpers. It is deliberately separate
from `memory/stdlib-io-format.md`, which records the `format`/`FormatBuffer`
design.

The canonical surface now ships as `stdlib/std/result.zith`: the positional
`pub union Result<T, E> { T, E }` plus `Ok`/`Err`/`isOk`/`isErr`/`unwrap`/
`unwrapErr` generic helpers. Result-only parameters infer from the expected
result type (a binding annotation or a declared return type), so `Ok(7)` and
`new(3)` bind `T`/`E` without an explicit `<...>` list. `test-stdlib-modules`
type-checks the module and `test-codegen` (`test_imported_result_runtime`) runs
an imported `Result<T, E>` end-to-end.

## Current Compiler Facts

Generic unions work for concrete instantiations:

```zith
union Result<T, E> {
    T,
    E,
}
```

The supported extraction path is tagged/raw union casting, not named variant
access:

```zith
let n: u32 = raw result as u32;
```

Free generic helpers that construct and return a generic union now compile.
The earlier blocker was in generic-union monomorphization: `substituteType`
built the concrete instance name from the template's unsubstituted members
(`T`/`E`), so every instance collapsed to `Result<T,T>`. It now names the
instance from the substituted members, so the helper below checks and runs.
`Ok`/`Err` remain plain generic helpers; they are not named union cases
(named cases are not part of the language). The C++ regression lives in
`test-enum-union-generics` (`test_generic_union_instances_stay_distinct`):

```zith
union Result<T, E> {
    T,
    E,
}

fn Ok<T, E>(value: T): Result<T, E> {
    var r: Result<T, E> = Result<T, E>{ value };
    return r;
}

fn main(): i32 {
    let a: Result<u32, u32> = Ok<u32, u32>(7);
    return 0;
}
```

`Result<void, E>` is intentionally out of scope for the first stdlib I/O
version. There is no validated `()` generic argument and `null` to `void`
would be speculative. The sink overload returns `IoError` instead.
Instantiating a generic union with `void` (for example `Result<void, E>`) is
now rejected with `E3001` at the use site instead of hanging LLVM codegen; see
`memory/sema-generic-union-void.md`.

## Fatal `panic`

`stdlib/std/panic.zith` ships the fatal `panic(message)`. It writes the message
and a trailing newline one byte at a time through `putchar`, then terminates
through the runtime panic code `R10004` (`@assert(false)` outside a test). The
native `__zith_runtime_panic` helper flushes the C streams (`fflush(nullptr)`)
before `llvm.trap`, so the message survives the abort even when the program's
output is a pipe. The VM v2 lowering accepts any `R10001+` code, so the same
source runs on both backends. `test-codegen` (`test_imported_panic_runtime`)
and `test-stdlib-modules` cover it.

## Adopted Surface

The first stdlib I/O version does not use a generic union for `format`.
`FormatResult` remains a plain struct that keeps the owned `FormatBuffer` and
an `IoError` status together:

```zith
pub struct FormatResult {
    buffer: FormatBuffer,
    status: IoError,
}
```

The owned `format(allocator, msg, values)` overload returns `FormatResult`.
Callers use `result.error()`, `result.text()`, and `result.destroy()`.
`Result<T, E>` helpers and variadic tail forwarding now work. `FormatResult`
is the current I/O surface choice, not a workaround for those compiler
limitations. The separate append overload writes into a caller-owned
`FormatBuffer` and returns `IoError`.

## Design Decision

Generic unions remain valid for concrete instantiations and direct union
construction plus `raw as` extraction:

```zith
Result<FormatBuffer, IoError>{ buffer }
Result<FormatBuffer, IoError>{ IoError.fWrite }
```

Generic union helper inference now works (see the `Ok`/`Err` example above), so
`Result<T, E>` helpers are shipped in `stdlib/std/result.zith`. The first I/O
version still keeps the plain `FormatResult` struct, so the I/O surface is
unchanged; `std/result` is available to new callers.
