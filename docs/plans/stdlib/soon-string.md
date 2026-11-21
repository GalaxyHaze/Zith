# `soon/string` Owning Byte String

Status: drawing

`stdlib/soon/string.zithm` ships an owning, heap-backed byte string with
`init`, `get`, `ptr`, `len`, `empty`, `iter`, `at`, `slice`, `equalsCString`,
`startsWith`, `concat`, and `destroy`. It is the first real `soon/` module and
the candidate reference example for a full draw-to-conform cycle. It currently
has no contract test and no signed promises.

This drawing fixes the string contract before the promotion gate in
[soon promotion](soon-promotion.md) can be applied.

Size: module, the owning byte-string surface.

## Current Facts

- The module owns a `data: ?*char`, a `length: u64`, and a stored
  `allocator: dyn Allocator`. It is null-terminated and also carries an
  explicit length, so it supports embedded NUL through `get`/`at`.
- It imports the raw C surface through `import "string.h"` and uses `strcpy`,
  `strcat`, `strcmp`, `memcmp`, and `strlen`. The supported `c/string` module
  only declares `memcpy`, `memset`, `strlen`, and `strncmp`, so the module does
  not yet route through the `c` surface.
- `get` and `slice` return a raw view over `data`, so the view aliases the
  owner. There is no separate `view string` type.
- `concat` returns `?Self` and checks `u64` overflow before allocating.

## Non-goals

- Does not add UTF-8 or text-encoding semantics. The string is bytes.
- Does not add `append`, `find`, `replace`, or formatting.
- Does not change the compiler or the `c/string` surface.
- Does not promise the allocator-retention contract, which is tracked in
  [allocator ownership](allocator-ownership.md).

## Open decisions

- Is `data` always null-terminated, or is the trailing byte an implementation
  detail? The answer fixes what `ptr()` may promise.
- Do `get` and `slice` return `view []char` or an owning copy, and how is the
  alias documented so a caller does not free through it?
- Does the module keep `import "string.h"`, or move to the `c/string` surface?
  The latter needs `strcpy`, `strcat`, and `strcmp` added to `c/string`.
- What is the `view string` form used by read-only methods, given that `get`
  currently hands out a raw slice?
- Which of `equalsCString`, `startsWith`, `concat` are part of the first
  promise set, and which stay provisional?
