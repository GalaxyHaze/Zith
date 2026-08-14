# Symbols Boundary

## Purpose

`src/symbols/` contains the declarative symbol generator and is a protected subsystem. Changes to
its rules, generator, or generated output require explicit user approval.

## Source Of Truth

- `symbols.rules` declares symbol kinds, data layout, and basic helper metadata.
- `generate.py` emits `build/src/symbols/symbols.hpp`.
- `src/symbols/CMakeLists.txt` wires the generated header and the `zct_symbols` interface target.

Do not edit `build/src/symbols/*`; it is generated output. The import graph in
`src/common/import/` consumes the generated symbol surface and is protected along with this
directory.

## Demo

There is no symbols demo target. The existing `tests/import/import-demo` exercises the generated
symbol API indirectly through the protected import graph; see `tests/import/README.md` for the
build and run commands.

## Agent Boundary

Do not modify anything under `src/symbols/` or `src/common/import/` without explicit user
approval. Documenting this boundary does not change the subsystem.
