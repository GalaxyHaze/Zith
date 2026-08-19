# Session 02 - Stabilize Generated AST and Parser Public Types

## Context

`src/frontend/ast/ast.rules` was already expanded to the migration node set and
`build/src/frontend/ast/ast.hpp` was generated with `Program`, `Declaration`,
`GenericParam`, `Parameter`, `ImportDecl`, `ImportPathSegment`, `ImportSelector`,
`TypeExpr`, `Expr`, `ExprField`, `Stmt`, and `Binding`.

The parser surface needs stable public types before the handwritten recursive
parser is implemented:

- `src/frontend/parser/parse.hpp` currently declares `parseSource()` but does not
  include `<vector>`.
- `src/frontend/parser/types.hpp` still has only the default `ParseOutput()`
  constructor and no source-preserving constructor.
- Tests need stable numeric/enumerated kind values for declarations, expressions,
  statements, and type expressions.

## Goal

Make the generated AST and parser output types a complete, compile-checkable API
for Session 03 without modifying any generator source.

## Out of Scope

Do not implement parsing behavior. Do not modify lexer rules. Do not add a
handwritten replacement AST. Do not touch `build/`.

## Files You May Edit

- `src/frontend/parser/types.hpp`
- `src/frontend/parser/parse.hpp`
- `tests/frontend/ast-test.cpp`
- `tests/frontend/CMakeLists.txt` only if an AST/parser type smoke test is added
  or renamed

## Files You May Only Read

- `src/frontend/ast/ast.rules`
- `build/src/frontend/ast/ast.hpp`
- `build/src/frontend/ast/ast.cpp`
- `build/src/frontend/ast/walk.hpp`

## Steps

1. Fix `src/frontend/parser/parse.hpp`:
   - Add `#include <vector>`.
   - Keep the public entry point:
     ```cpp
     [[nodiscard]] sample::ParseOutput parseSource(
         generated_parser::Parser<sample::ParseOutput> &parser,
         generated_lexer::TokenStream &tokens,
         std::string_view source,
         std::vector<sample::ParserDiagnostic> *outDiagnostics = nullptr);
     ```
2. Update `src/frontend/parser/types.hpp`:
   - Add an explicit source constructor to `ParseOutput`:
     ```cpp
     explicit ParseOutput(std::string_view sourceValue)
         : ast(arena), source(sourceValue) {}
     ```
   - Keep move-only semantics and the default constructor required by
     `generated_parser::Parser` static assertions.
3. Add stable kind enums in `namespace sample`. Use `int` leaves in the
   generated AST so tests and Session 03 can cast these enums to `int`.
   Suggested minimums:
   ```cpp
   enum class DeclKind : int {
       Error, Import, Function, Variable, Struct, Enum, Union, TypeAlias,
       Trait, Interface, Marker, Macro, Word, Context
   };
   enum class VisibilityKind : int { Private, Public, Module };
   enum class FunctionKind : int {
       Standard, Extern, Const, Raw, Flow
   };
   enum class TypeExprKind : int {
       Error, Name, Pointer, Optional, Slice, Array, Function, Opaque
   };
   enum class ExprKind : int {
       Error, Literal, Name, Unary, Binary, Assign, Call, Index, Field,
       Arrow, OptionalProp, Cast, StructLiteral, ArrayLiteral, Block,
       If, While, For, When, Range, Placeholder, MacroCall,
       LayoutIntrinsic, IsNull
   };
   enum class StmtKind : int {
       Expression, Binding, Return, Break, Continue, Dock, Marker, Jump
   };
   ```
   Keep ownership as `int` too; suggested values:
   `0 Default`, `1 Lend`, `2 Share`, `3 View`, `4 Unique`, `5 Belong`.
   Put the definitions after the includes and before `ParseOutput`.
4. Audit `ast.rules` against the generated header:
   - Confirm arrays are declared with `= children`.
   - Confirm child pointers use `= child`.
   - Confirm only supported leaf types: `Span`, `int`, `std::string_view`,
     `bool`, `uint64_t`, and existing node names.
   - Do not change the generator. If a required leaf cannot be represented, stop
     and ask for explicit generator approval.
5. Update `tests/frontend/ast-test.cpp` to allocate and exercise all generated
   node kinds:
   - `Program`
   - `Declaration`
   - `GenericParam`
   - `Parameter`
   - `ImportDecl` with a path segment and selector
   - `TypeExpr`
   - `Expr`
   - `ExprField`
   - `Stmt`
   - `Binding`
   - Verify `nodeCount()`, walk traversal, clone into a fresh `AstRoot`, and
     `print()` on the migrated node set.
6. Syntax-compile the public surface:
   ```bash
   g++ -std=c++23 -fsyntax-only \
     -Isrc -Ibuild/src \
     -Isrc/frontend/parser -Isrc/frontend/lexer -Isrc/frontend/ast \
     -Ibuild/src/frontend/parser -Ibuild/src/frontend/lexer -Ibuild/src/frontend/ast \
     src/frontend/parser/types.hpp
   ```
7. Run AST and parser type smoke tests:
   ```bash
   cmake --build build --target ast-test -j
   ctest --test-dir build -R '^ast-' --output-on-failure
   ```

## Acceptance Criteria

- `parse.hpp` compiles with `<vector>` included.
- `ParseOutput(std::string_view)` is available and move-only.
- Stable kind enums are defined for parser/tests.
- Generated AST supports walk, clone, and print for every node in the migration
  set.
- No generator source is modified.

## Expected Next State

Session 03 can construct generated AST nodes and return `ParseOutput` safely.
