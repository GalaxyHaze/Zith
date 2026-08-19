# Session 01 - Reconcile Lexer Token Kinds and Parser Rules

## Context

An earlier agent added four stock lexer token kinds (`Import`, `From`, `Export`,
`Asset`) to `src/frontend/lexer/generate.py`. The migration plan forbids
modifying generator source. The goals of this session are to remove that
generator edit and to remap the imported-keyword surface onto the existing stock
token kinds, then keep lexer/parser generation consistent.

Current relevant state:

- `src/frontend/lexer/generate.py` contains:
  ```python
  TOKEN_KINDS = [
      "Identifier",
      "As",
      "Import",
      "From",
      "Export",
      "Asset",
      "Using",
      ...
  ```
- `src/frontend/lexer/lexer.rules` defines keyword groups named `Import`,
  `From`, `Export`, and `Asset`.
- `src/frontend/parser/parser.rules` lists `Import action=...`, `From action=...`,
  `Export action=...`, and `Asset action=...`.
- `tests/frontend/lexer-test.cpp` may assert the four non-stock keyword kinds.

## Goal

Keep the lexer generator byte-identical to the repository baseline while still
tokenizing `import`, `from`, `export`, and `asset` as keywords.

## Out of Scope

Do not implement the recursive parser here. Do not change AST rules. Do not
change session stages. Do not touch `build/`.

## Files You May Edit

- `src/frontend/lexer/lexer.rules`
- `src/frontend/parser/parser.rules`
- `tests/frontend/lexer-test.cpp`

## Files You Must Restore / Not Edit

- `src/frontend/lexer/generate.py`: leave byte-identical to the baseline; the
  accidental prior edit must not be re-introduced.
- `src/frontend/parser/generate.py`: leave untouched.
- `src/frontend/ast/ast.rules`: leave untouched in this session unless an AST
  test dependency blocks lexer work, in which case treat it as a Session 02
  item instead.

## Steps

1. Read the current `git diff` for `src/frontend/lexer/generate.py` and confirm
   it is clean.
2. Edit `src/frontend/lexer/lexer.rules`. Replace the four keyword groups with
   one semantic mapping on the stock `Using` kind:
   ```text
   Using = ["use", "import", "from", "export", "asset"]
   ```
   Keep all other current keyword groups.
3. Edit `src/frontend/parser/parser.rules`:
   ```text
   Using action=hooks::parser::top()
   ```
   Remove the four `Import`, `From`, `Export`, `Asset` rule lines. Keep
   `top()` inert; `parseSource()` in Session 03 will inspect lexemes directly
   from the token stream.
4. Update `tests/frontend/lexer-test.cpp` so all expected import/export/asset
   keyword assertions use `TokenKind::Using` or assert lexemes instead of the
   removed token kinds:
   - `import` -> `TokenKind::Using`
   - `from` -> `TokenKind::Using`
   - `export` -> `TokenKind::Using`
   - `asset` -> `TokenKind::Using`
5. Regenerate the lexer and parser surfaces using the documented commands:
   ```bash
   python3 src/frontend/lexer/generate.py src/frontend/lexer/lexer.rules --out build/src/frontend/lexer
   python3 src/frontend/parser/generate.py src/frontend/parser/parser.rules --out build/src/frontend/parser --types src/frontend/parser/types.hpp
   ```
6. Build the lexer and parser targets, then run the lexer focused tests:
   ```bash
   cmake --build build --target zct_frontend_lexer zct_frontend_parser -j
   ctest --test-dir build -R '^lexer-' --output-on-failure
   ```

## Acceptance Criteria

- `git diff -- src/frontend/lexer/generate.py` is empty.
- `src/frontend/lexer/lexer.rules` does not reference non-stock token kinds.
- `src/frontend/parser/parser.rules` does not reference non-stock token kinds.
- `import`, `from`, `export`, and `asset` tokenize as `TokenKind::Using`.
- All existing lexer tokenization tests pass.
- No files under `build/` are hand-edited.

## Expected Next State

The generated lexer has only the stock token kinds. The generated parser still
has only the minimal `TopLevel` cursor, and `top()` remains inert. Session 02
can then stabilize the parser surface without depending on removed token kinds.
