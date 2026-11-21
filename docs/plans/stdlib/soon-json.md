# `soon/json` Parser And Emitter

Status: drawing

`stdlib/soon/json.zithm` is a stub. It declares `parse`, `stringify`, and
`isValid` with no signatures and a `mod fn test` placeholder. JSON is the
first data-format module proposed for `soon/`, and it needs a value model
before any signature can be written.

This drawing fixes that value model and the text boundary, so the module can be
promised against the gate in [soon promotion](soon-promotion.md).

Size: module, the JSON data-format surface.

## Problem

JSON has to represent objects, arrays, strings, numbers, booleans, and null in
a language whose current stdlib has no owning map for arbitrary string keys and
no `Result<T, E>` surface. Generic `Ok`/`Err` helpers over the canonical
positional `union Result<T, E> { T, E }` now compile (see
[dynarray-result-panic](dynarray-result-panic.md)), so the failure channel is
available; the stub cannot be filled until the representation of an object is
decided.

## Non-goals

- Does not add a general serialization framework or derive macros.
- Does not depend on `std/collections/hash_map` string-keyed support, which
  does not exist yet.
- Does not promise streaming or incremental parsing.
- Does not fix a stable ABI, because this is a `soon/` contract.

## Open decisions

- What is the value model: a tagged union with an object as a pair of parallel
  slices, a linked entry list, or a string-keyed map once one exists?
- Does `parse` report failure through a generic `Ok`/`Err` helper over
  `union Result<T, E> { T, E }`, or through an owned result type like the I/O
  format surface?
- Is `parse` in-memory over `[]char`, or does it take the owning
  `soon/string` type?
- What does `stringify` accept, and is the output an owned string, a
  caller-provided buffer, or a `FormatBuffer`?
- Are numbers kept as text or converted to a fixed numeric type, and how is
  precision preserved?
- Is `isValid` a separate scan or a `parse` that discards its result?
