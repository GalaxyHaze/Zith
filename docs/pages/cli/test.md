---
id: cli-test
title: zithc test
section: CLI Reference
output: cli/D-test.html
aliases: cli/D-test.html
kind: editorial
---
# `zithc test`

Discover and run tests under a path.

```bash
zithc test
zithc test tests/
```

Each test target is compiled and executed, and the command's exit status reflects the run. Because tests are ordinary Zith programs, they are limited to the features the compiler implements today; see [Implementation Status](doc:reference-implementation-status).
