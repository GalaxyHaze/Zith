# Stdlib I/O Format Design

This note records the current `std/io` format implementation and the compiler
facts that constrain the facade plan. `stdlib/std/io/format.zith` is the real
formatting module; `stdlib/std/io/console.zith` remains the real module for
`print`/`println`/`input` and exposes the old HIR surface used by tests.

## Target Layout

- `stdlib/std/io/format.zith`: `Formatable`, `FormatBuffer`, `FormatResult`,
  `IoError`, the owned `format` free function, and primitive `Formatable`
  implementations.
- `stdlib/std/io/console.zith`: `InputLine`, `ParseInput`, `input()`, and the
  existing `print`/`println` wrappers. It imports `std/io/format` with `from`.

## Public Format Surface

```zith
pub trait Formatable {
    fn format(self, dest: lend FormatBuffer): IoError;
}

pub struct FormatResult {
    buffer: FormatBuffer,
    status: IoError,
}

pub fn emptyFormatBuffer(allocator: dyn Allocator): FormatBuffer;
pub fn format(dest: lend FormatBuffer, msg: []char,
              values: [...]dyn Formatable): IoError;
pub fn format(allocator: dyn Allocator, msg: []char,
              values: [...]dyn Formatable): FormatResult;
```

Both buffer construction and the owned `format` overload receive an allocator
explicitly. `FormatBuffer` retains it for growth and destruction. The owned
result must be destroyed with `result.destroy()`. `text()` returns the view
slice and `error()` returns the `IoError` status. The current module does not
provide an implicit `HeapAllocator` overload.

The placeholder grammar stays the same as `print`/`println`: `#` marks one
positional value and `\#` is the literal escape. `FormatBuffer` can grow; there
is no fixed-slice sink yet.

## Intended Target Surface

The domain contract for a sink is `TextSink`. `Formatable` can later accept
that contract so primitives can append through a reusable sink instead of only
through `FormatBuffer`:

```zith
pub trait TextSink {
    fn capacity(self): u64;
    fn length(self): u64;
    fn append(self: lend dyn TextSink, chars: []char): IoError;
    fn text(self): []char;
}

pub trait Formatable {
    fn format(self, dest: lend dyn TextSink): IoError;
}
```

This is the target recorded in `docs/adr/0022-stdlib-io-format-split.md`.
Migrating the stdlib to it remains follow-up work.

## Probe Facts Recorded

`TextSink` is the intended target contract for sinks. The compiler accepts a
`self: lend dyn TextSink` receiver and mutation through a `dyn` value updates
the caller's storage (ADR-0037 plus the receiver slice of issue #64).
`FormatBuffer` remains the concrete sink in the current stdlib surface because
migrating `Formatable` to `lend dyn TextSink` is separate follow-up work. A
qualified receiver such as `self: view Self` is still rejected with `E2022`.

Calling a method directly on a variadic `dyn Formatable` element does not
compile today:

```zith
let s = raw values[i].format();
```

The current stdlib bridge still works:

```zith
fn formatNext(value: dyn Formatable, buffer: lend FormatBuffer): IoError {
    return value.format(lend buffer);
}
```

`format` keeps this bridge shape and calls
`formatNext(raw values[next], lend buffer)`.

The owned result uses a plain struct to keep the buffer and status together.
Generic `Result<T, E>` helpers now work, but the current I/O surface has not
been changed to use them. Variadic tail forwarding is implemented. The
`print`/`println` path still has its own formatting loop, so that duplication
is current structure rather than a compiler limitation.

`export std/io/format` cannot preserve `std.io.console.println` qualified
access while also re-exporting the format symbols without duplicate `std`
bindings. The documented facade split remains a compiler debt; for now
`from std/io/format` is the supported bare import path.
