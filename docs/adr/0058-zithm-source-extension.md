# Zith-- Source File Extension

Status: accepted and implemented

The Zith-- frontend reads only files whose extension is `.zithm`. The shared
`.zith` extension is reserved for the flagship Zith language. This ADR records
the extension split, why it is the cheap moment to make it, and how the
language is chosen once both frontends live in one binary.

## Context

The compiler binary is `zithc` and today it implements only Zith--, so every
source file used the shared `.zith` extension. Zith and Zith-- are sibling
languages with different frontends, and Zith will be developed in parallel as
a prototype before the two converge. When Zith is acceptable and Zith-- is
concluded, the two frontends are linked into one `zithc` binary and Zith
becomes the default that `.zith` names.

Two problems follow from a single shared extension:

- Once both frontends are linked, a bare `.zith` is ambiguous. The C/C++
  model resolves this through a per-project default plus an explicit form, and
  that requires each language to own a distinct spelling.
- The compiler compares extensions in several places. An earlier import-namespace
  helper stripped the suffix with a hard-coded length (`size() - 5U`), which
  silently breaks if the extension length changes.

## Decision

- `.zithm` is the canonical source extension for Zith--. The `m` marks the
  minus form. Zith-- reads only `.zithm`, so `.zith` stays free for Zith.
- `.zith` is reserved for Zith. After the frontends converge, `.zith` selects
  Zith by default and `.zithm` stays the explicit Zith-- override, the way
  `.c` and `.cpp` disambiguate in C/C++.
- The extension lives in one place. `src/support/source-extension.hpp` defines
  `kZithmSourceExtension` with `isZithmSource`, `hasZithmSourceExtension`, and
  `stripZithmSourceExtension` helpers. No call site spells the extension as a
  literal or a magic length.
- The compiler's own artifacts keep their names. The persistent cache directory
  (`.zith-cache`), the documentation sidecar prefix (`.zith-<kind>-<id>`), and
  the `zith_*` ABI symbols are not source files and are not renamed.

## Consequences

- A `.zith` file is no longer compiled by this frontend. The split was made
  during beta, before release assets and downstream installs pinned the
  extension, so the migration cost stays in-tree.
- Language selection for the converged binary is by extension, with a project
  manifest default and a command-line override. The dispatch stays a dimension
  of the existing CLI command set (`check`, `build`, `run`, `fmt`).
- A future `.zith` and `.zithm` file pair for the same module is a deliberate
  language difference, not an ambiguity to infer from file contents.
