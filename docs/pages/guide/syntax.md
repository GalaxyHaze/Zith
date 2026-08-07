---
id: guide-syntax
title: Syntax
section: Language Guide
output: guide/D-syntax.html
aliases: language/D-syntax.html
kind: editorial
---
# Syntax

Zith source uses braces for blocks and semicolons for ordinary statements. A function body may end with an expression, which becomes its return value.

```zith
fn add(a: i32, b: i32): i32 {
    a + b
}
```

Use `//` for a line comment.

## Operators

Arithmetic and comparison use the familiar spellings: `+ - * / %` and `== != < > <= >=`. Logical operators are the keywords `and`, `or`, `not`, and `xor`. Writing `&&` or `||` is a dedicated error that points you at `and` / `or`, reported exactly once with no cascade.

Bitwise operators keep a trailing dot to stay distinct from the logical keywords: `&.` for AND, `|.` for OR, `^.` for XOR, with unary `~` for NOT. Both operands must be integers of the same type.

```zith
fn bits(a: i32, b: i32): i32 {
    let both: i32 = a &. b;
    let either: i32 = a |. b;
    let diff: i32 = a ^. b;
    ~(both + either + diff)
}
```

Compound assignment works for `+= -= *= /= %= <<= >>= &= |= ^=`. The parser desugars each one into an ordinary assignment, so a compound assignment yields a value and inherits the same coercion and `view` rules as `=`. Note that the bitwise compounds drop the dot of their base spelling.

There are no increment or decrement operators; write `i += 1`.

## Literals

Integer literals accept explicit radix prefixes: `0x` hexadecimal, `0c` octal, and `0b` binary. Digit separators such as `1_000` are not supported, and a literal wider than 64 bits reports `E0004`. String and character literals decode the usual C-style escapes; an unknown escape reports `E0001`.

```zith
fn radix(): i32 {
    0xFF + 0c17 + 0b101
}
```

Write a declaration before its use, and keep the first programs within the working core. See [Functions](doc:guide-functions) and the formal [Function reference](doc:reference-05-functions).
