# Frontend Helper

## Purpose

The `frontend` directory is the home of source-level compiler components. It contains the lexer
helper, which generates token tables and tokenization support code from `lexer.rules`, and the AST
helper, which generates node structs, allocation, walking, and printing from `ast.rules`.

The generated code should be treated as build output. Changes belong in the rules file or the
generator, not in the generated headers or sources.

## Subsystems

| Path | Role |
|---|---|
| `frontend/README.md` | Entry point for frontend helpers. |
| `frontend/lexer/` | Declarative lexer helper and generated lexer. |
| `frontend/ast/` | Declarative AST node helper and generated node surface. |

See the top-level `readme.md` and `frontend/lexer/` or `frontend/ast/` documentation for the full
workflow.
