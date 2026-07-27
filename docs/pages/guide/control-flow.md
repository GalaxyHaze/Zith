---
id: guide-control-flow
title: Control Flow
section: Language Guide
output: guide/D-control-flow.html
aliases: language/D-control-flow.html
kind: editorial
---
# Control Flow

`if`, `when`, and `for` are working control-flow constructs. Use `if` for a boolean branch and `for` for iteration.

```zith
fn sign(value: i32): i32 {
    if (value < 0) {
        -1
    } else {
        1
    }
}
```

The language also specifies chains, markers, docks, and jumps. The latter are specialized tools; consult the [Control Flow reference](doc:reference-09-control-flow) before using them.
