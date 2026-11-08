# Stdlib I/O Format Split

## Status

Accepted as scoped implementation; module split deferred.

## Context

`std/io/console` currently mixes `input()`/`InputLine`/`ParseInput` with
`print`/`println`/formatting. The stdlib needs clearer seams: reading from a
console should not depend on the format contract, writing should make
formatting reusable by user types, and current examples/tests must not break.
At the time of this decision, variadic forwarding between stdlib functions
was rejected. That compiler limitation has since been removed. Qualified
facade re-export still cannot preserve `std.io.console.*`.

The existing `Formatable` trait and `#`-placeholder runtime format are the
stable parts of the current console path. `FormatBuffer` owns allocator-backed
storage and is reused by `print`/`println`. The proposed change adds a sink
abstraction so formatting can write into `FormatBuffer` or a caller-provided
fixed slice. The compiler now supports mutable `dyn`/`lend` receivers for the
intended `TextSink` abstraction, but the stdlib migration has not been done.

## Decision

Keep the split scoped and deferred to the compiler-facade debt. In this
iteration, `format.zith` owns `Formatable`, `FormatBuffer`, `FormatResult`,
`IoError`, and the owned free `format`. `console.zith` remains the real module
for `InputLine`, `ParseInput`, `input()`, and the existing `print`/`println`.
The future split into `consoleIn`/`consoleOut` plus a qualified facade is
recorded in `docs/implementation-debt.md` and `CONTEXT.md`.

The adopted owned result is the current `format` surface. Generic
`Ok`/`Err` helper inference now works, so retaining `FormatResult` is a stdlib
choice rather than a compiler requirement:

```zith
pub trait Formatable {
    fn format(self, dest: lend FormatBuffer): IoError;
}

// Intended for the later TextSink migration:
// pub trait TextSink {
//     fn append(self: lend dyn TextSink, chars: []char): IoError;
// }
// pub trait Formatable {
//     fn format(self, dest: lend dyn TextSink): IoError;
// }

pub struct FormatResult {
    buffer: FormatBuffer,
    status: IoError,
}

pub fn format(allocator: dyn Allocator, msg: []char,
              values: [...]dyn Formatable): FormatResult;
```

The current owned `format` overload accepts an allocator explicitly, and
`FormatBuffer` retains it for growth and destruction. There is no implicit
`HeapAllocator` overload in this surface.

## Consequences

Current console examples and tests keep compiling with their old HIR names.
`format` is available through `from std/io/format`; qualified facade access via
`std.io.console.*` remains a documented compiler debt. Generic `Ok`/`Err`
helpers, mutable `dyn`/`lend` receivers, and variadic tail forwarding are now
available. Migrating `Formatable` to `TextSink` remains stdlib work, not a
compiler blocker. `print`/`println` still use their own formatting loop even
though variadic forwarding is available.
