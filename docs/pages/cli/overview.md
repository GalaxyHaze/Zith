---
id: cli-overview
title: CLI Reference
section: CLI Reference
output: cli/D-overview.html
aliases: cli/D-cli.html
kind: editorial
---
# CLI Reference

This reference documents the working `zithc` commands for Zith--, as recorded
in the canonical [Implementation Status](doc:reference-implementation-status).
The Zith language reference describes a separate design; it does not define
these commands. Command options change with the compiler, so use
`zithc --help` for the executable you installed.

## Working commands

- [`zithc build`](doc:cli-build)
- [`zithc run`](doc:cli-run)
- [`zithc check`](doc:cli-check)
- [`zithc fmt`](doc:cli-fmt)
- [`zithc create`](doc:cli-create)
- [`zithc clean`](doc:cli-clean)
- [`zithc execute`](doc:cli-execute)
- [`zithc test`](doc:cli-test)
- [`zithc deps`](doc:cli-deps) (`list`, `add`, `remove`, `resolve`)
- [`zithc docs`](doc:cli-docs)
- `zithc completion <shell>` (bash, zsh, fish)

## Stubs

`zithc repl` is a stub and prints "not implemented yet".

## Common options

`--emit obj|ir|asm|hir` stops the pipeline before linking and writes the named intermediate instead. `--cache-stats` prints object-cache hit and miss counts for the build. `zithc run` forwards the compiled program's output to zithc's own stdout, while compiler diagnostics stay on stderr, so you can pipe program output without capturing diagnostics.
