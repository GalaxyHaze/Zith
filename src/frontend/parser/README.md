# Parser Helper

## Purpose

The parser helper generates a declarative token-driven parser from `parser.rules`.
The generated `Parser<Output>` owns cursor navigation, context-stack handling, rule
matching, diagnostics, and action dispatch while handwritten hooks implement syntax
behavior in `actions.cpp`.

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

`[contexts]` declares every valid context and derives `ParserState` from it. `TopLevel` is required.
`[parser]` declares the C++ types visible to generated code and used by `types.hpp`. Each
`[context Name parent=...]` section contains token rules; `parent=` names the exact stack chain
under which that context applies.

```text
[parser]
input: std::string_view
output: sample::ParseOutput
diagnostic: sample::ParserDiagnostic
tokenStream: generated_lexer::TokenStream
end: Token::End
onError: hooks::parser::recover()

[contexts]
TopLevel
Module

[context TopLevel]
Fn lexeme="fn" push=Module action=hooks::parser::beginModule()

[context Module parent=TopLevel]
Identifier lexeme="name"
```

Rule syntax is `kind[, kind...] [lexeme="..."] [punc="..."] [push=Context] [pop=Context]
[action=hooks::parser::foo()]`.

- `kind` may be one or more token kinds separated by commas.
- `lexeme=` and `punc=` filter on the current token when present.
- `push=` and `pop=` change the context stack after a successful action. `TopLevel` is never
  removed, and `pop=` only applies when the current top equals the listed state.
- `action=` names a hook declared in `actions.hpp` and implemented in `actions.cpp`.
- `builder=true` on a context enables the parser's `OutputBuilder` for hooks attached to that
  context and requires every enabled context to have at least one rule action.

Context declarations:

- `[parser] end: Token::End` makes `End` terminate the stream silently in every context before local
  rules run. Omitting it keeps the previous per-context `End` rules.
- `[parser] onError: hooks::parser::recover()` installs a `Recovery recover(Parser &, const Token &)`
  hook. `Recovery::Skip` advances the token without adding the automatic `unexpected token`
  diagnostic; `Recovery::Abort` stops the parse. If `Parser::abort()` was called by a hook, parse
  stops even when recovery returns `Skip`.
- `[context Name parent=A,B]` matches only when the current stack is `Name,A,B` from top to bottom.
  Parent chains are explicit, may only name contexts declared in `[contexts]`, and may not overlap
  with another section of the same child.
- `[parser] builder: CustomBuilder<Output>` selects a custom builder type when at least one context
  uses `builder=true`. Without it, the generated parser uses
  `common::parser::OutputBuilder<Output>`.

`OutputBuilder` owns a stack of `Output` values. Hooks can build a tree by pushing values, then
calling `builder().attach()` or `builder().close(N)`. `attach(parent, child)` is found by ADL in
the user's output type namespace and must be available whenever the builder is enabled:

```cpp
namespace sample {
struct ParseOutput {
    int count = 0;
};

void attach(ParseOutput &parent, ParseOutput child) {
    parent.count += child.count;
}
} // namespace sample
```

Generated action signatures use concrete aliases plus the configured output type:

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
