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

Use `//` for a line comment. Write a declaration before its use, and keep the first programs within the working core. See [Functions](doc:guide-functions) and the formal [Function reference](doc:reference-05-functions).
