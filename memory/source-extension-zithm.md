# Source Extension: `.zithm`

The Zith-- frontend reads only `.zithm` source files. The shared `.zith`
extension is reserved for the flagship Zith language, which will be developed
in parallel and later linked into the same `zithc` binary. See
[ADR-0058](../docs/adr/0058-zithm-source-extension.md) for the decision.

## Where the extension lives

`src/support/source-extension.hpp` owns `kZithmSourceExtension` plus
`isZithmSource`, `hasZithmSourceExtension`, and `stripZithmSourceExtension`.
Never spell `.zithm` as a literal in a new decision site and never use a
hard-coded suffix length. The previous `moduleNamespace` strip used
`size() - 5U`, which silently breaks if the extension length changes.

## Decision sites

- Import resolution appends the extension for generic modules, `mod`, and
  platform variants: `src/session/frontend-module-analysis.cpp`.
- File discovery for `zithc test` and `zithc fmt`: `src/cli/cmd/tool.cpp`,
  `src/cli/cmd/fmt.cpp`.
- Project scaffolding writes `src/main.zithm`: `src/cli/cmd/project.cpp`.
- Module namespace stripping: `src/sema/hir-lower-utils.cpp`.
- WASM virtual sources and the stdlib pack: `src/wasm/playground.cpp`,
  `scripts/package-stdlib.py`.

## Not renamed

`.zith-cache` (persistent cache), `.zith-<kind>-<id>` (docs sidecar prefix),
and the `zith_*` ABI symbols are compiler artifacts, not sources, so they keep
their names. A rename sweep must use a lookahead that leaves `.zith-`,
`.zithc`, and `zith_` untouched.
