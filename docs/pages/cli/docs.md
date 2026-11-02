---
id: cli-docs
title: zithc docs
section: CLI Reference
output: cli/D-docs.html
aliases: cli/D-docs.html
kind: editorial
---
# `zithc docs`

Generate documentation from comments attached to Zith-- declarations.

```bash
zithc docs src/main.zith
```

The command requires input files; invoking it with nothing to read reports "no input files". Doc comments (`///` and `/** */`) attached to declarations are the input, so document declarations at their definition site.

## Document a module

Keep the source file in the input list and place the comment immediately before
the declaration it describes:

```zith
/// Adds two signed integers.
fn add(left: i32, right: i32): i32 {
    left + right
}
```

Pass every source file that contributes public declarations. The command does
not infer a whole project from a directory, and it does not replace the
project's language reference.
