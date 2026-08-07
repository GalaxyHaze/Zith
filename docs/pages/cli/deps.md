---
id: cli-deps
title: zithc deps
section: CLI Reference
output: cli/D-deps.html
aliases: cli/D-deps.html
kind: editorial
---
# `zithc deps`

Inspect the dependencies declared in `ZithProject.toml`.

```bash
zithc deps list
```

`list` is the implemented subcommand; it reads the project manifest and prints the declared dependencies. Running it outside a project reports that `ZithProject.toml` was not found.

`deps add` and `deps remove` are stubs that print "not implemented yet". Edit `ZithProject.toml` directly until they land.
