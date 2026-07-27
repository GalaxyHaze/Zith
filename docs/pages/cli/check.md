---
id: cli-check
title: zithc check
section: CLI Reference
output: cli/D-check.html
aliases: cli/D-check.html
kind: editorial
---
# `zithc check`

Type-check files or a project without emitting output.

```bash
zithc check
zithc check src/main.zith
```

This is the fastest command to run after changing source code. A successful check only covers features implemented by the current compiler; see [Implementation Status](doc:reference-implementation-status).
