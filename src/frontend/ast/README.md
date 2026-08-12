# AST Helper

## Purpose

The AST helper generates node structs, allocation helpers, a DFS walker, and a tree printer from
`ast.rules`. It keeps AST node wiring declarative instead of hand-writing repetitive struct and
traversal code.

The generated files are build output. Edit `ast.rules` or `generate.py`, then regenerate.

## Files

| File | Responsibility |
|---|---|
| `ast.rules` | Node sections and field declarations. |
| `generate.py` | Reads `ast.rules` and emits the AST C++ surface. |
| `types.hpp` | User-owned include surface consumed by generated output. |
| `build/src/frontend/ast/ast.hpp` | Generated node structs, `AstRoot`, `make`, `alloc`, `free`. |
| `build/src/frontend/ast/ast.cpp` | Generated `AstRoot` methods and `print`. |
| `build/src/frontend/ast/walk.hpp` | Generated DFS `walk` traversal. |

## Rules Syntax

Each uppercase header is one node type. Leaf fields can be written without a
tag; `child`, `children`, `string`, and `value` remain supported for clarity.

```text
[Program]
body: common::memory::DynArray<Expr> = children

[Expr]
span: Span

[LiteralExpr]
span: Span
valueText: std::string_view

[CallExpr]
span: Span
callee: Expr = child
name: std::string_view
arguments: common::memory::DynArray<Expr> = children
```

Supported tags:

- `child`: one generated node pointer.
- `children`: a `common::memory::DynArray<DeclaredNode>` list of generated node pointers.
- `string`: optional marker for `std::string_view` leaf data.
- `value`: optional marker for scalar or `Span` leaf data.

Leaf fields default to leaf data when no tag is present; they must be
`std::string_view`, `Span`, or a supported scalar type. `child`/`children`
fields must reference declared node types. `walk` visits node fields in
declaration order; `print` prints leaf fields on the node line and children
indented below it.

## Regenerate

```bash
python3 src/frontend/ast/generate.py \
  src/frontend/ast/ast.rules \
  --out build/src/frontend/ast
```

## Tests

- `ast-generated-basics` runs allocation, walk parent/order, print, and free smoke checks.
- `ast-generator-regression` validates rule rejection and compiles the generated surface.

## Agent Boundary

Edit `ast.rules` for node and field declarations and `types.hpp` for user-owned type surface.
Do not edit `build/src/frontend/ast/*`. Do not modify `generate.py` or shared generator rules
without explicit user approval.
