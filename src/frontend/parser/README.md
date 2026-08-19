# Parser Helper

## Purpose

The parser helper generates a stable token-cursor and recovery surface from
`parser.rules`. The generated `Parser<Output>` owns cursor navigation,
diagnostics, and the `TopLevel` dispatch loop. Zith's recursive grammar lives in
handwritten `actions.cpp`; `hooks::parser::parseSource()` drives the parsed
surface directly and builds generated AST nodes instead of relying on generated
rule-by-rule reductions.

The showcase parser recognizes source-located import declarations:

```text
import parent/sample;
import parent/sample/ as Dir;
from parent/sample { render as draw, log };
export parent/sample(2);
import "stdio.h" { printf };
import assets/dat.json as Json;
import asset assets/dat.json as Json;
```

Import parsing records the raw path, path segments/spans, selector aliases, form flags,
asset/header classification, depth, and declaration span in `sample::ParseOutput.imports`.
It only performs syntax validation; filesystem and `parent/sample`
file-vs-folder decisions remain resolver-owned.

## Files

| File | Responsibility |
|---|---|
| `parser.rules` | Parser type, context, parent-chain, and rule declarations. |
| `generate.py` | Reads `parser.rules` and emits the parser C++ surface. |
| `types.hpp` | User-owned `Span`, `input`, `output`, and `diagnostic` declarations. |
| `actions.cpp` | Handwritten hook implementations declared in rules. |
| `build/src/frontend/parser/parser.hpp` | Generated public parser API. |
| `build/src/frontend/parser/parser.cpp` | Generated translation-unit anchor. |
| `build/src/frontend/parser/actions.hpp` | Generated action declarations. |

## Rules Syntax

`[parser]` declares the C++ types visible to generated code and used by
`types.hpp`. `[contexts]` currently declares the single stable `TopLevel`
context; every input token kind routes to `hooks::parser::top()`, which is a
no-op. The recursive parse is initiated inline by `parseSource()` and consumes
`parser.tokenStream()` directly.

```text
[parser]
input: std::string_view
output: sample::ParseOutput
diagnostic: sample::ParserDiagnostic
tokenStream: generated_lexer::TokenStream
end: Token::End

[contexts]
TopLevel
```

The generator still supports the declarative rule forms (`kind`,
`lexeme=`, `punc=`, `push=`, `pop=`, `action=`, context parents, builders, and
`onError`) for smaller parsers. Zith uses only the `TopLevel` dispatch surface
by design; grammar behavior is in `actions.cpp`.

Generated action signatures use concrete aliases plus the configured output
type:

```cpp
using Parser = generated_parser::Parser<sample::ParseOutput>;
using Token = generated_lexer::Token;

void foo(Parser &parser, const Token &token);
Recovery recover(Parser &parser, const Token &token);
```

## Public API

The generated `Parser<Output>` exposes:

- `tokenStream()` returning the configured `TokenStreamAlias`, so callers use
  `tokenStream().advance()`, `tokenStream().match(...)`, `tokenStream().hasNext()`, and the
  other `TokenStream` APIs directly.
- cursor helpers: `current`, `peek`, `slice`.
- token queries: `lexeme`, `slice`, `span_slice`, `lookupState`.
- context state: `stack`, `topState`, `pushState`, `popState`, `abort`.
- diagnostics and output: `diag`, `diagnostics`, `error`, `output`, `setOutput`.
- builder state: `builder()` returning the configured `BuilderAlias` when `builder=true` is used.
- `parse(TokenStream &, Input source = {})` returning `common::memory::Result<Output, Diagnostic>`.

`hooks::parser::parseSource(parser, tokens, source)` is the showcase entry point:
it resets the parser on the given token stream, builds `Program` nodes, records
recoverable diagnostics in `sample::ParseOutput`, and leaves the generated loop
untouched.

## Regenerate

```bash
python3 src/frontend/parser/generate.py \
  src/frontend/parser/parser.rules \
  --out build/src/frontend/parser \
  --types src/frontend/parser/types.hpp
```

## Tests

- `parser-generated-basics` runs the compiled parser smoke test.
- `parser-generator-regression` validates the generator against temporary rules and compiles the
  generated surface.

## Agent Boundary

Edit `parser.rules` for parser/context/rule declarations, `types.hpp` for user-owned type
surface, and `actions.cpp` for syntax behavior. Do not edit `build/src/frontend/parser/*`. Do not
modify `generate.py` or shared generator rules without explicit user approval.

## Demo

`tests/frontend/parser-demo.cpp` parses the base showcase grammar and the import
forms, then prints the parse status, declaration count, and import count.

```bash
cmake --build build --target parser-demo -j
ctest --test-dir build -R '^parser-demo$' --output-on-failure
```
