# Session 04 - Add and Rework Parser Parity Tests

## Context

`tests/frontend/parser-test.cpp` and `tests/frontend/parser-demo.cpp` are stale;
they reference removed `ParserState::Module`, `ParserState::Type`, builder APIs,
and old `ParseOutput` fields such as `count` and `sawEnd`.

`src/frontend/parser/actions.cpp` is expected to be implemented in Session 03.

## Goal

Replace the stale parser test surface with parity tests that exercise the
generated lexer, generated AST, and handwritten `parseSource()` parser.

## Out of Scope

Do not port sema, macro expansion, symbols, import resolution, or codegen tests.
Do not modify parser generators.

## Files You May Edit

- `tests/frontend/parser-test.cpp`
- `tests/frontend/parser-demo.cpp`
- `tests/frontend/CMakeLists.txt`

## Test Setup Pattern

Each test should:

1. Create a `common::memory::StringInterner` and arena.
2. Call `generated_lexer::tokenize(source, interner)`.
3. Construct `generated_parser::Parser<sample::ParseOutput> parser(arena)`.
4. Call `hooks::parser::parseSource(parser, tokens, source)`.
5. Assert on `output.ast.root`, node kinds/counts, spans, `imports`, and
   `diagnostics`.

Do not call `parser.parse()`. Use `parser.tokenStream().reset()` when reusing a
stream, or recreate the stream each test.

## Required Coverage

### Lexer-Level Assertions Through the Parser

- All keyword lexemes tokenize correctly.
- Full operator/compound list is present:
  `-> .. == != <= >= += -= *= /= %= <<= >>= &= |= ^= ~> && || << >> |. &. ^. || := ...`
- Slash handling in import paths and comments.

### Declarations

- Function kinds: `fn`, `const fn`, `raw fn`, `extern fn`, `flow fn`.
- Visibility: default, `pub`, `mod`.
- `extern` before declarations.
- Imports: path, `as`, from selectors, depth, header string, asset form.
- Structs, components, enums, unions, aliases, traits, interfaces.
- Generic declarations and parameters.
- Macro declarations and tag macros.

### Expressions and Statements

- Binary precedence and associativity.
- Unary operators.
- Calls, index, field, arrow, cast, generic call, `is null`.
- `for` variants, deprecated `while`.
- `when`/`match`, ranges, `~>`, default `_`.
- Arrays, struct literals, blocks.
- Bindings, return, break, continue, dock, marker, jump.
- Unsupported `use`, word-operator sequences, failable/fallback operators.

### Diagnostics and Recovery

- `fn broken(}` reports a source-located delimiter/parse diagnostic.
- Consecutive garbage such as `@@@` produces one coalesced diagnostic.
- `$ $ $ fn ok() { }` still collects `fn ok` after garbage.
- Delimiter errors report spans, not a hard abort.

### AST API

- Walk the returned AST.
- Clone it into a fresh `AstRoot`.
- Call `print()` on a non-empty AST.
- Assert span preservation for a representative declaration and expression.

## CMake Target Guidance

- Keep or create a target named `parser-generated-basics` or `parser-test`.
- Prefer adding a dedicated `parser-parity-test` executable so focused runs are:
  ```bash
  ctest --test-dir build -R '^parser-' --output-on-failure
  ```
- Make sure the target links `zct_frontend_parser`, `zct_frontend_ast`,
  `zct_frontend_lexer`, and `zct_common`.

## Verification Steps

```bash
cmake --build build --target parser-test parser-demo -j
ctest --test-dir build -R '^parser-' --output-on-failure
```

If a target name differs after inspecting `tests/frontend/CMakeLists.txt`,
use the actual target names in the focused ctest run.

## Acceptance Criteria

- All stale references to removed `ParserState` values and old `ParseOutput`
  fields are gone.
- Parity tests cover every syntax category listed above.
- Assertions include AST shape, spans, declaration/expression/statement counts,
  diagnostic counts/spans, and recovery behavior.
- Focused parser tests pass from a clean focused run.

## Expected Next State

Session 05 can trust the parser output and focus on session wiring.
