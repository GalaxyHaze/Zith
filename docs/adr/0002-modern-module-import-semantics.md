# Modern Module Import Semantics

The modern pipeline treats `import path` as a pure namespace binding, `import path as name` as an explicit module alias, `from path` as direct injection of public symbols, and `export path` as re-exporting both the full-path namespace and the public symbols to consumers. Qualified access must work in expressions, types, constructors, and method resolution; a plain import never injects symbols into the current scope.

Status: accepted

Considered Options:

An older hybrid behavior injected imported symbols globally to make simple examples compile. That broke namespace hygiene and duplicated declarations. Another option was to make `import path` identical to `from path`; that would violate the documented module-system distinction and make collisions common. The accepted behavior follows the spec while keeping `from` as the deliberate escape hatch.

Consequences:

- Parser, sema, type lowering, and HIR resolution must represent multi-segment qualified names.
- `import std/console` exposes `std.console.println`, not bare `println`; callers use `from std/console` when they want bare names.
- `export` requires propagating both alias-like namespace edges and public symbols through the resolution graph.
