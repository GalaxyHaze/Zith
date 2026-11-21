# No Increment/Decrement and One Dots Token

## Status

Accepted and implemented in Zith--.

## Context

The `++` and `--` operators were considered as conveniences over explicit assignment, but they introduce ambiguity with lexing, compound assignment, and update semantics for little benefit in this subset. Zith-- already treats values as immutable by default and documents explicit assignment as the canonical update form.

The `..` and `...` spellings previously travelled through the lexer as two or three punctuation tokens. This forced the parser to special-case dot adjacency for ranges, pipeline placeholders, relative imports, module depth, and variadic slices.

## Decision

Zith-- removes `++` and `--`. The lexer is updated to emit `TokenKind::Dots` for both `..` and `...`, with the lexeme distinguishing the intended use. Increment and decrement forms are lexed as operators only so the frontend can reject them with a clear diagnostic pointing to explicit assignment.

Range `1..<4` still uses the `..` spelling, `|> f(..)` still uses `..` as the pipe placeholder, `from ../lib` still uses a relative parent segment, and `mod(..)` still means unlimited module depth. `...` marks variadic declarations and macro repetition. In a call, `f(...values)` explicitly forwards one final slice or array to a variadic-slice parameter. See [ADR-0060](0060-explicit-variadic-slice-forwarding.md).

## Consequences

The frontend no longer needs adjacency checks for dot tokens in most expressions and imports. Tests cover `..` and `...` as one token, range and slice parsing, pipeline placeholders, import paths and depths, struct field `mod(..)`, and rejection of both postfix and prefix `++`/`--`.

Any future reintroduction of increment or decrement operators requires a deliberate spec change and must pass the full compiler pipeline again.
