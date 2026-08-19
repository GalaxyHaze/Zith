# Frontend Helper

## Purpose

The `frontend` directory is the home of source-level compiler components. It contains the lexer
helper, which generates token tables and tokenization support code from `lexer.rules`, the AST
helper, which generates node structs, allocation, walking, and printing from `ast.rules`, and the
parser helper, which generates a token-driven parser surface from `parser.rules`.

The generated code should be treated as build output. Changes belong in the rules file or the
generator, not in the generated headers or sources.

## Subsystems

| Path | Role |
|---|---|
| `frontend/README.md` | Entry point for frontend helpers. |
| `frontend/lexer/` | Declarative lexer helper and generated lexer. |
| `frontend/ast/` | Declarative AST node helper and generated node surface. |
| `frontend/parser/` | Declarative parser helper and generated parser surface. |

See the top-level `readme.md` and `frontend/lexer/` or `frontend/ast/` documentation for the full
workflow.

## Architecture

The frontend is intentionally a thin index for three generated helpers:

- The lexer consumes `lexer.rules` and emits a `Lexer`, `Token`, `TokenKind`, and `TokenStream`.
- The AST helper consumes `ast.rules` and emits `AstRoot`, node types, allocation helpers, walking,
  cloning, and printing.
- The parser helper consumes `parser.rules` and emits `Parser<Output>`, context
  rules, recovery, and hook declarations. The showcase parser implementation
  lives in `frontend/parser/actions.cpp`.

Generated output lives under `build/src/frontend/*`; handwritten behavior belongs in each helper's
`types.hpp` or `actions.cpp`. There is no standalone frontend demo target because each helper has
its own demo.

## Demos

Each frontend helper has a small executable that exercises its public API:

| Demo target | CTest | Demonstrates |
|---|---|---|
| `lexer-demo` | `lexer-demo` | Tokenizing keywords, identifiers, punctuation, operators, strings, and numeric literals. |
| `ast-demo` | `ast-demo` | Allocating and printing a `Program -> Expr` tree. |
| `parser-demo` | `parser-demo` | Parsing showcase snippets and inspecting declarations/imports. |

```bash
cmake --build build --target lexer-demo ast-demo parser-demo -j
ctest --test-dir build -R '^lexer-demo$|^ast-demo$|^parser-demo$' --output-on-failure
```

Use the per-helper commands in each subdirectory when only one demo is needed.
