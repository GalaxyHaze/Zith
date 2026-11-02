---
id: cli-deps
title: zithc deps
section: CLI Reference
output: cli/D-deps.html
aliases: cli/D-deps.html
kind: editorial
---
# `zithc deps`

Manage the local dependencies declared in `ZithProject.toml`.

```bash
zithc deps list
zithc deps add <name> <relative-path>
zithc deps remove <name>
zithc deps resolve
```

`list` reads the project manifest. `add` and `remove` change local path
dependency declarations; resolution is explicit. `resolve` walks the
transitive local dependency graph and writes a deterministic `Zith.lock`.
Project `check`, `build`, and `run` require a lockfile that matches the
manifest.
