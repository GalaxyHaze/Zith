# Standard Library I/O and Format Contract

The first user-facing stdlib I/O surface is `print`/`println`/`input` backed by the `Formatable` and `ParseInput` traits. `print` and `println` take a `[]char` format message plus a homogeneous variadic tail of `dyn Formatable` values; `#` in the message marks one positional value and `\#` is the literal escape. `input()` returns an owning `InputLine` wrapper with `text`, `ok`, `cast<T>`, and `destroy`; resource errors are carried by a plain `IoError` enum instead of a failable result because Zith-- avoids `T!` in this stdlib contract.

Status: accepted (print/println/input and Formatable)

Considered Options:

- Using `string` or `*char` as the message type. The current compiler models string literals as `*char` with an implicit `[]char` adapter; `[]char` keeps a length and avoids mixing raw pointers with format parsing.
- Using `T!`/`void!` for output failures. Zith-- intentionally keeps the stdlib surface simple and the user explicitly rejected failable error unions here; a named `IoError` result is idiomatic and stable.
- Treating placeholders as compile-time syntax. The lexer already decodes `\#` into a literal `#`, so a runtime parser alone cannot know which `#` was escaped; the contract therefore introduces an internal sentinel for `\#` so the runtime scanner can distinguish a literal mark from a placeholder.
- Restricting `Formatable` to aggregates. `dyn` erasure already exists for structs/enums/unions, but supporting primitives requires extending erasure to `bool`, `f32`, `f64`, `u32`, and `*char`; this ADR explicitly includes that compiler change.

Consequences:

- `Formatable`, `FormatBuffer`, `ParseInput`, `IoError`, and `InputLine` become public stdlib contracts documented in `CONTEXT.md` and `docs/20-standard-library.md`.
- `print`/`println` own a reusable `FormatBuffer` per call, append chunks, then write once (with `\n` added by `println`).
- `InputLine` owns its allocated buffer and requires `destroy(self: lend InputLine)`; parsing is delegated to primitive `ParseInput` implementations.
- `ParseInput` is documented as the future parsing contract, but is not shipped
  yet: Zith-- does not yet type-check generic trait methods such as
  `T.parse(self): ?Self`, so `InputLine.cast<T>` is recorded as implementation
  debt alongside this ADR.
- A compiler change is required so primitives can be erased to `dyn Formatable`; without it the variadic signature cannot accept primitive values.
- Missing placeholder arguments are not fatal: a placeholder with no value renders `{absent}` and the call returns `IoError.Ok`.
- Error variants use the `f` prefix convention (`fAllocation`, `fWrite`) for the standard library.
- Normal string and char literals still decode `\#` to `#`; `decodeEscapes` exposes `keep_marker` so a format-message lowering path can preserve the sentinel instead.
