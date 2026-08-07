---
id: cli-docs
title: zithc docs
section: CLI Reference
output: cli/D-docs.html
aliases: cli/D-docs.html
kind: editorial
---
# `zithc docs`

Generate documentation from Zith source.

```bash
zithc docs src/main.zith
```

The command requires input files; invoking it with nothing to read reports "no input files". Doc comments (`///` and `/** */`) attached to declarations are the input, so document declarations at their definition site.
