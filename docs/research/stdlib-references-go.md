# Go Standard Library: Reference Notes

**Access date:** 2026-10-31

Source baseline: local Go 1.26.7 tree, paths below are relative to
`$GOROOT/src` (GOROOT = the Homebrew `go/1.26.7/libexec`). On this access date,
the official version endpoint identifies Go 1.27.3 as current. It was released
on 2026-10-19. Its JSON API is covered by the official release notes and
package documentation below, not by the local source inspection. These sources
are prior art for the Zith stdlib, not a spec to copy.
[`Go version`](https://go.dev/VERSION)

## 1. Package organization

One flat namespace, one directory per package, no inheritance. Layering is
enforced by the `internal/` rule: a package under `internal/` is importable only
by code rooted at its parent (`internal/stringslite/strings.go:5-6`). The result
is a dependency DAG that bottoms out in leaf packages.

- `internal/bytealg` holds the byte/string primitives (`Index`, `Compare`,
  `Count`, `HashStr`) with per-architecture assembly beside them
  (`internal/bytealg/bytealg.go:7-8`; `*_amd64.s`, `*_arm64.s`). It exposes
  `MaxLen` and `PrimeRK` (`bytealg.go:29-33`) so callers can pick a search
  strategy.
- `internal/stringslite` is a subset of `strings` that "only us[es] packages
  that may be imported by os" (`internal/stringslite/strings.go:5-7`). It exists
  to break a cycle: `os` needs string helpers but sits below `strings`.
- `unicode/utf8` defines its own `RuneError`/`MaxRune`/`UTFMax` "to avoid this
  package depending on package unicode" (`unicode/utf8/utf8.go:11-21`).

Responsibility split: `strings` operates on immutable `string` and is the home
of Unicode-aware behavior (`strings/strings.go:5-13`). `bytes` is "analogous to
the facilities of the strings package" but over mutable `[]byte`
(`bytes/bytes.go:5-7`). `strconv` converts textual to numeric/bool forms and
quotes literals (`strconv/doc.go:5-55`), and is not a string-manipulation
package. `unicode/utf8` decodes/encodes runes and validates UTF-8
(`unicode/utf8/utf8.go:5-6`; `DecodeRune` `:157`, `Valid` `:470`). `unicode`
holds the tables and `RangeTable` (`unicode/letter.go`, `unicode/tables.go`).

`vendor/` is separate: copies of external modules needed by std and cmd, renamed
with a `vendor/` prefix so package paths stay distinct (`README.vendor:1-12`).
It is a build-time closure, not a public surface.

## 2. The string/bytes duality

Near-parallel: 48 exported funcs in `strings/strings.go`, 52 in
`bytes/bytes.go`, roughly 47 names shared verbatim (`Contains`, `Index`,
`Split`, `Trim*`, `ToUpper`, `Replace`, ...). The duplication is deliberate: a
`string` is immutable and copy-free to slice, so `strings.Index` returns a
substring, while `[]byte` is mutable and owned, so `bytes` must respect aliasing
and carries mutators like `bytes.Buffer`.

What is shared is pushed down rather than factored into a common generic core:

- Both import `internal/bytealg` (`strings/strings.go:12`, `bytes/bytes.go:11`).
- `strings.Compare` is a one-line `bytealg.CompareString` call
  (`strings/compare.go:12-14`). `bytes.Compare` calls `bytealg.Compare`
  (`bytes/bytes.go:28-29`).
- `strings.Count` special-cases a single-byte needle to `bytealg.CountString`
  (`strings/strings.go:44-46`).

The cost is real: two copies of every algorithm, two doc surfaces, and generic
text code must pick one or abstract over both (`bytealg.HashStr[T string |
[]byte]`, `bytealg.go:36`). Go accepts it because a single generic element
abstraction would make the common `string` path allocate.

## 3. Error handling model

`error` is a one-method interface defined in `builtin`:
`type error interface { Error() string }` (`builtin/builtin.go:317-318`). There
are no exceptions. Fallible functions return an `error` last, and callers
branch on it.

- `func New(text string) error` returns a distinct `*errorString` per call
  (`errors/errors.go:63-66`). `Unwrap` calls a single `Unwrap() error`
  (`errors/wrap.go:17-25`), `Is` walks the tree checking identity and any
  `Is(error) bool` method (`:45-72`), and `As` finds the first assignable error
  (`:102`), with `AsType[E error](err error) (E, bool)` avoiding the out-param
  (`:167`). `Join(errs ...error)` exposes non-nil children via
  `Unwrap() []error` (`errors/join.go:20`, `:61`).

Wrapping is a formatting convention, not a language feature: `fmt.Errorf` with
`%w` produces an `Unwrap`, and several `%w` verbs produce `Unwrap() []error`
(`fmt/errors.go:16-22`, `:83-92`). "Wraps" is structural: any type with
`Unwrap() error` or `Unwrap() []error` participates (`errors/errors.go:9-27`).
Sentinel errors are package-level `var Err... = errors.New(...)`, e.g.
`io.EOF` (`io/io.go:44`) and `io.ErrUnexpectedEOF` (`io/io.go:48`). Matching uses
`Is`, not `==`, so wrapping never breaks callers.

The model rests on panics being reserved for programmer error. Recover exists
but is used narrowly, for example `bytes.growSlice` turning an allocation panic
into `ErrTooLarge` (`bytes/buffer.go:247-251`).

## 4. I/O interfaces

`io` is the canonical small-interface composition. Each is one method
(`io/io.go`): `Reader` is `Read(p []byte) (n int, err error)` (`:86-88`),
`Writer` is `Write(p []byte) (n int, err error)` (`:99-101`), `Closer` is
`Close() error` (`:107-109`), `Seeker` is `Seek(offset int64, whence int)
(int64, error)` (`:126-128`).

Combinations embed the small ones, they do not inherit:
`ReadWriter { Reader; Writer }` (`:131-134`), `ReadCloser` (`:137`),
`ReadWriteSeeker` (`:176`). Optional capabilities are separate interfaces probed
at runtime: `ReaderFrom`, `WriterTo`, `ReaderAt`, `StringWriter` (`:189-311`).
Algorithms are free functions over the interfaces, not methods:

- `func ReadFull(r Reader, buf []byte) (n int, err error)` is
  `ReadAtLeast(r, buf, len(buf))` (`io/io.go:353-355`), which loops until `min`
  bytes or error and converts mid-stream `EOF` to `ErrUnexpectedEOF` (`:329-351`).
- `func Copy(dst Writer, src Reader)` dispatches to `src.(WriterTo)` then
  `dst.(ReaderFrom)`, else a 32 KiB staged loop (`:387-389`, `copyBuffer`
  `:407-459`).
- `func WriteString(w Writer, s string)` uses `StringWriter` when available,
  else `w.Write([]byte(s))` (`:314-319`).

Duck typing plus capability probing lets any type satisfy `io.Writer` without
naming `io`, which is why `fmt.Fprintf`, `os.File`, and `bytes.Buffer`
interoperate with no shared base type.

## 5. Generic collections and algorithms

- `slices`: generic free functions over `~[]E` (`Equal`, `Compare`, `Index`,
  `Contains`, `Insert`, `Delete`, `Clone`, `Compact`, `Reverse`, `Concat`,
  `Repeat`, `slices/slices.go:20-512`) plus sorting (`Sort`, `SortFunc`,
  `SortStableFunc`, `BinarySearch`, `slices/sort.go:16-152`).
- `maps` is tiny: `Equal`, `Clone`, `Copy`, `DeleteFunc`
  (`maps/maps.go:17-69`).
- `cmp` supplies the `Ordered` constraint and NaN-consistent `Less`/`Compare`
  (`cmp/cmp.go:12-40`).
- `sort` is the legacy interface API: `Len()/Less(i, j)/Swap(i, j)`
  (`sort/sort.go:17-48`), with `Ints`/`Strings` now delegating to `slices.Sort`
  (`:170-181`).

`iter` (Go 1.23) defines push-style iterators as function types:
`type Seq[V any] func(yield func(V) bool)` and
`type Seq2[K, V any] func(yield func(K, V) bool)` (`iter/iter.go:223-229`). A
sequence stops early when `yield` returns false. `iter.Pull`/`Pull2` convert
push to pull with `next`/`stop` closures backed by runtime coroutines, using
`runtime.newcoro` and `runtime.coroswitch` (`iter/iter.go:231-261`).
Collections expose `iter.Seq` views:
`slices.All`/`Backward`/`Values` (`slices/iter.go:14-40`),
`maps.All`/`Keys`/`Values` (`maps/iter.go:11-40`), and
`strings.Lines`/`SplitSeq` (`strings/iter.go:16-40`).

`container/*` is the pre-generics, `any`-based fallback: `list` (doubly linked,
`any` `Value`, `container/list/list.go:5-24`), `ring` (circular,
`container/ring/ring.go:5-17`), and `heap` (an interface over `sort.Interface`,
`container/heap/heap.go:31-38`). All three lose type safety and pay boxing.

## 6. JSON APIs in Go 1.27

Go 1.27 adds `encoding/json/v2` for semantic conversion between JSON and Go
values. Its APIs support byte slices, readers and writers, and configurable
options. By default, v2 rejects invalid UTF-8 and duplicate object names.
Existing `encoding/json` remains supported, so migration is optional. The
separate `encoding/json/jsontext` package handles syntax-level processing
through `Token` and raw `Value` streams. Its encoder and decoder keep internal
state to validate that emitted and consumed tokens form valid JSON text.
`Value` stores raw JSON bytes, but writing one still validates its syntax and
may reformat it according to encoder options. That state is internal to syntax
validation, not a public workflow state machine or async control flow.

For Zith, the transferable point is the separation of syntax processing from
typed conversion, not Go's reflection-based mapping or implicit interface
behavior. [`Go 1.27 release notes`](https://go.dev/doc/go1.27#standard_library),
[`encoding/json/v2`](https://pkg.go.dev/encoding/json/v2), and
[`encoding/json/jsontext`](https://pkg.go.dev/encoding/json/jsontext).

## 7. Strings.Builder and byte-buffer growth

`strings.Builder` wraps a `[]byte` and hands it out as a `string` with no copy
via `unsafe.String(unsafe.SliceData(b.buf), len(b.buf))`
(`strings/builder.go:17-18`, `:46-48`). Because that aliases, it forbids
copying: `copyCheck` panics on a non-zero builder copied by value
(`:32-41`). The zero value is usable and writes are `append`-based (`:87-115`).
Later writes append after the returned string's prefix, and `Reset` drops the
buffer rather than overwriting it (`:58-61`, `:85-115`).

Growth is exponential: `grow` allocates `2*cap(b.buf) + n` and copies
(`strings/builder.go:66-71`). `bytes.Buffer` uses the same 2x policy but keeps a
read offset so it is both reader and writer, allocating `len(b)+n` or `2*cap(b)`,
whichever is larger, and grows by at least `MinRead = 512` per `ReadFrom`
(`bytes/buffer.go:144-171`, `:214-227`, `:247-262`). Costs worth naming: 2x
over-allocates up to double at the peak. `Builder.String` relies on unsafe
aliasing, but its append-only API preserves already-returned prefixes.
`Buffer` relies on `recover` to turn allocation failure into `ErrTooLarge`.

## 8. Lessons for Zith

Transfers well:

- One-method interfaces composed by embedding. Zith's `dyn Trait` fat pointers
  express `Reader`/`Writer`/`Closer` directly, and `copy(dst: dyn Writer, src:
  dyn Reader)` is the right shape. The capability probe (`src.(WriterTo)`) maps
  to a trait-object downcast, so Zith needs a cheap "implements trait T" check.
- Structural error wrapping. `Unwrap` plus `Is`/`As` is an interface method and a
  tree walk, no language feature required. It can complement Zith's
  `soon/result` if Zith error values later expose a wrapped cause.
- Leaf-internal packages to break cycles. A `stringslite`-style subset is the
  proven fix when a low-level module needs text helpers but cannot depend on the
  full string library.
- Free functions over collections (`slices`, `maps`) rather than methods, which
  keeps generic dispatch at the function level.

Does not transfer, or transfers with a tax:

- The `string`/`bytes` duplication. Zith has no immutable `string` with
  copy-free slicing, so importing both packages doubles the surface without the
  payoff. A single byte-slice package plus a distinct owned-string type is more
  honest.
- GC 2x growth. In an arena, `grow` cannot assume a collector reclaims the old
  block, so reallocation either leaks the old block until arena reset or chains
  blocks. Copy the policy, not the assumption.
- `unsafe` aliasing in `Builder.String`. Go can expose it because the builder
  only appends and never overwrites the returned prefix. Zith should still
  express the relationship through `view`/`lend`, rather than expose an
  untracked alias.
- `iter.Seq`'s closure protocol depends on capturing mutable state and, for
  `Pull`, on runtime coroutines. An explicit iterator trait (`next() ->
  Optional[V]`) fits Zith's ownership rules better than push closures.
- `container/list`, `ring`, `heap` over `any` are pre-generics artifacts. Ship
  them generic from the start, or omit `list`/`ring` until demand exists.
- `panic`/`recover` for recoverable conditions (`ErrTooLarge`). Zith has no
  exceptions, so allocation failure must surface through `Result` or abort.
