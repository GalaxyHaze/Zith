# Lexer Helper

## Purpose

The lexer helper generates a lexer implementation from `lexer.rules`. It replaces repetitive token
table construction and lexer plumbing with a compact declarative description.

The source of truth is `lexer.rules`, supported by `generate.py`. Handwritten behavior that is not
table wiring belongs in `actions.cpp` or in consumer code, not in generated files.

## Files

| File | Responsibility |
|---|---|
| `lexer.rules` | Token, keyword, lexer-member, token-member, and action declarations. |
| `generate.py` | Reads `lexer.rules` and emits the lexer C++ surface. |
| `types.hpp` | User-owned include surface and declarations consumed by the generated output. |
| `actions.cpp` | Hand-written hook implementations declared by `[actions]`. |
| `build/src/frontend/lexer/lexer.hpp` | Generated public lexer API. |
| `build/src/frontend/lexer/lexer.cpp` | Generated tables and lexer implementation. |
| `build/src/frontend/lexer/actions.hpp` | Generated declarations for hooks referenced by the rules. |
| `build/src/frontend/lexer/keyword-table.hpp` | Generated keyword lookup table. |

## Rules Syntax

### `[tokens]`

The token section configures built-in token scanning:

```text
[tokens]
identifier = true
skip = " \n\t\r"
string  = [escape-sequence = true, back-slash = true]
decimal = [under-score-divisor = true]
hexadecimal = true
binary = true
octal = true
punc = "{}()[]:;.|!?"
operators = "+-*/%<>&^"
compound = ["+=", "-=", ">>=", "<<=", "*=", "/="]
comments = [single = "//", multi = ["/*", "*/"]]
```

`compound` emits explicit multi-character operators. Do not rely on implicit arrow or other
compound inference.

### `[keywords]`

Each keyword maps a token kind name to one or more source spellings:

```text
[keywords]
If = "if"
Else = "else"
When = "when"
Fn = "fn"
```

### `[lexer]`

Optional generated members and methods on the lexer object:

```text
[lexer]
# add fields or methods here
```

### `[token-type]`

Optional generated fields and methods on the token structure:

```text
[token-type]
channel: int = 0
```

If token-type fields are added, an `[actions] onToken` hook is required because the generated
lexer needs a hook to populate those fields.

### `[actions]`

Supported hooks are `onLex`, `offLex`, and `onToken`. They are emitted into `actions.hpp` and must
be implemented in `actions.cpp`:

```text
[actions]
onLex = hooks::lexer::onLex()
onToken = hooks::lexer::onToken()
offLex = hooks::lexer::offLex()
```

If `[token-type]` declares a field, `onToken` is mandatory because the generated lexer uses the hook
to populate the extra token member.

## Common Workflow

1. Read `src/frontend/lexer/lexer.rules` before changing the lexer.
2. Edit `lexer.rules` first.
3. Regenerate the lexer:

```bash
python3 src/frontend/lexer/generate.py \
  src/frontend/lexer/lexer.rules \
  --out build/src/frontend/lexer \
  --types src/frontend/lexer/types.hpp
```

4. Update `src/frontend/lexer/actions.cpp` only if the rules changed hooks.
5. Rebuild and run the tests:

```bash
cmake --build build -j
ctest --test-dir build -R lexer --output-on-failure
```

## Hooks

Hook declarations are generated from `[actions]`. The supported implementation point is
`src/frontend/lexer/actions.cpp`. Keep hook implementations focused on behavior, not on generated
table structure.

## Tests

- `lexer-generated-basics` runs the compiled lexer smoke test.
- `lexer-generator-regression` runs the generator against temporary rule files and compiles the
  resulting hook surface.

Run both with:

```bash
ctest --test-dir build -R lexer --output-on-failure
```
