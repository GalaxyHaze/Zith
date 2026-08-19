# Session 03 - Implement Handwritten Recursive Parser Actions

## Context

`src/frontend/parser/actions.cpp` is currently deleted. The generated parser is
a minimal token cursor with only `TopLevel` and an inert
`hooks::parser::top()`. The plan explicitly says recursive grammar belongs in
handwritten `actions.cpp`, while the generated parser remains the token cursor.

Reference semantics are in the `main` branch:

- `/home/diogo/Zith/src/frontend/frontend.cpp`
- `/home/diogo/Zith/tests/test-frontend.cpp`

## Goal

Recreate `src/frontend/parser/actions.cpp` so `parseSource()` walks the generated
token stream, builds generated AST nodes in an arena-backed `ParseOutput`, and
records source-located diagnostics without failing the generated `Parser`.

## Out of Scope

- Sema, symbols, imports resolution, macro expansion, HIR, codegen.
- Generator changes.
- `FrontendSnapshot` compatibility.
- Session integration (Session 05).

## Files You May Edit

- `src/frontend/parser/actions.cpp` (recreate)
- `src/frontend/parser/parse.hpp` only if the entry signature needs adjustment
- `src/frontend/parser/types.hpp` only to add parser-internal constants if needed
- `src/frontend/parser/CMakeLists.txt` if it is missing required AST linkage

## Files You May Read

- `build/src/frontend/parser/parser.hpp`
- `build/src/frontend/ast/ast.hpp`
- `build/src/frontend/lexer/lexer.hpp`
- `/home/diogo/Zith/src/frontend/frontend.cpp`
- `/home/diogo/Zith/tests/test-frontend.cpp`

## Implementation Contract

1. `top(Parser &, const Token &)` must remain a no-op; `parseSource()` drives the
   recursive parse manually.
2. `parseSource()` must:
   - Accept the already-constructed `generated_parser::Parser<sample::ParseOutput>`,
     the generated lexer token stream, a `std::string_view source`, and an
     optional diagnostics out-vector.
   - Construct `ParseOutput(source)`.
   - Build a `generated_ast::Program` root in `output.ast`.
   - Move the supplied token stream into the parser cursor (`parser.reset(tokens,
     source)`) and drive `parser.tokenStream()` manually.
   - Advance `parser.tokenStream()` manually until `cur().kind == TokenKind::End`
     or recovery aborts.
   - Use `parser.span_slice(token.span)` for all lexemes and leaves, preserving
     raw source reconstruction.
   - Append every generated declaration to `output.ast.root->body`.
   - Append recoverable parser diagnostics to `output.diagnostics` rather than
     `parser.diag()`. Do not call `parser.parse()`.
   - Return `ParseOutput` by value.
3. Use `common::memory::DynArray<AstNode *>` arrays initialized by generated
   constructors. Do not pass arrays to `generated_ast::make`.
4. Do not call `std::abort` on malformed syntax. Emit diagnostics and recover.
5. Coalesce consecutive top-level garbage into one diagnostic, matching `main`
   behavior. Valid declarations after garbage must still be collected.
6. Preserve declaration, expression, and statement spans over the exact source
   range consumed.

## Required Grammar Coverage

### Top Level

- Visibility: `pub`, `mod`, default private.
- Pending `extern`.
- Declarations: functions, variables, structs, components, enums, unions, aliases,
  traits, interfaces, markers, macros, implements/impl blocks, imports.
- Function-kind prefixes: `fn`, `const fn`, `raw fn`, `extern fn`, `flow fn`.
- Macro forms: `macro`, `raw macro`, `tag macro`, and top-level `@name` calls.
- Coalesced garbage recovery.

### Imports

- `import path;`
- `import path/ as Alias;`
- `from path { name as alias, name2 };`
- `export path(...)`
- `import "header.h" as H;`
- `import assets/... as X;`
- `import asset assets/... as X;`
- Path segments, `..`, depth `(...)`, selectors, aliases, spans.
- Populate both generated `ImportDecl` nodes and parallel
  `ParseOutput::imports` metadata.

### Types

- Name, pointer (`*T`), optional (`?T`), slice (`[]T`), array (`[N]T`),
  function types, opaque (`raw opaque`).
- Ownership prefixes: `lend`, `share`, `view`, `unique`, `belong`.
- `mut` qualifiers, duplicate/contradictory qualifier diagnostics.
- Generic type arguments where the AST allows them.

### Expressions

- Peak precedence parser: assignment, comparison, bitwise `|.`/`^.`/`&.`,
  shifts, additive, multiplicative, unary.
- Binary operators: `== != < > <= >= << >> + - * / % & | ^`
- Unary operators: `- ! ~ not & *`
- Assignment and compounds: `= += -= *= /= %= <<= >>= &= |= ^=`
- Postfix: calls, index, `.field`, `->field`, casts via `as`, generic calls,
  optional propagation `?`, unsupported failable `!`.
- Literals, names, `true`, `false`, `null`, `unknown`, `invalid`.
- Struct literals and field mixing diagnostics.
- Array literals.
- Blocks and scopes.
- `if`/`else`, `while` with deprecation diagnostic, `for { }`,
  `for (cond) {}`, 3-clause `for`, unsupported iterator `for x in ...`.
- `when`/`match` with `~>` cases, ranges `lo..hi`, default `_`.
- Macro calls, tag macros, layout intrinsics.
- `is null`; unsupported non-null `is` forms.
- Unsupported `&&`, `||`, prefix `?`/`!`, and word-operator sequences with the
  same diagnostic intent as `main`.

### Statements

- Bindings: `let`, `var`, `const`; typed and initialized.
- `return`, `break`, `continue`.
- `dock target(args);`, `marker name(...) { }`,
  `stackful marker name(...) { }`, `jump target(args);`.
- Unsupported `use` statements with diagnostics.

## Suggested Internal Structure

In `actions.cpp`, keep helpers local to `namespace hooks::parser`:

- A small cursor wrapper: `advance()`, `peek()`, `text()`, `atPunc(char)`,
  `atOp(string_view)`, `atLexeme(string_view)`, `diagnose(Span, string_view)`.
- Declaration dispatch: `parseTopLevel(Parser&, ParseOutput&)`.
- Import builder: `parseImport(...)`.
- Declaration builder: `parseDeclaration(...)`, `parseFunctionKind(...)`.
- Type parser: `parseType(...)`.
- Expression parser: `parseExpression(...)`, `parsePrimary(...)`,
  `parsePostfix(...)`.
- Statement/body parser: `parseStatement(...)`, `parseBlock(...)`.
- Recovery helpers: `skipToDeclarationBoundary()`, `coalesceGarbage()`.

## Verification Steps

1. Syntax-compile the implementation:
   ```bash
   g++ -std=c++23 -fsyntax-only \
     -Isrc -Ibuild/src \
     -Isrc/frontend/parser -Isrc/frontend/lexer -Isrc/frontend/ast \
     -Ibuild/src/frontend/parser -Ibuild/src/frontend/lexer -Ibuild/src/frontend/ast \
     src/frontend/parser/actions.cpp
   ```
2. Ensure `src/frontend/parser/CMakeLists.txt` links `zct_frontend_ast` if
   `actions.cpp` needs it in the real target build.
3. Build the parser target:
   ```bash
   cmake --build build --target zct_frontend_parser -j
   ```
4. Before Session 04 replaces parser tests, at minimum make `parser-demo`
   compile if it is wired to `parseSource()`; otherwise note it as stale.

## Acceptance Criteria

- `actions.cpp` compiles in the project build.
- `parseSource()` returns generated AST rooted at `Program`.
- Well-formed declarations/expressions/statements listed above produce AST nodes.
- Malformed input produces source-located diagnostics and still permits later
  valid declarations.
- `parser.diag()` is not used for recoverable parse errors.
- No generator source is modified.

## Expected Next State

Session 04 can compile and assert against a real `ParseOutput`.
