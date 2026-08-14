# Import Graph

## Purpose

`src/common/import/` owns the handwritten import graph used to connect modules and resolve visible
symbols. It is a protected subsystem: changes to the implementation require explicit user
approval. This README documents the existing public API and how to exercise it without modifying
the protected code.

## Architecture

`ImportGraph` holds an arena and a string interner. It owns module nodes, dependency edges, and
per-module symbol/scope storage. The graph supports:

- module creation and lookup;
- dependency edges between modules;
- module finalization and cycle detection;
- resolution guards for recursive load/resolve flows;
- ancestor/distance queries used by module-scoped visibility;
- local symbol lookup and all-visible-name iteration.

`Module` exposes local declarations, scopes, lookups, iteration, and symbol metadata. Symbol kind,
visibility, `SymId`, and `SymbolData` come from the generated `src/symbols` surface.

## Public API

```cpp
ImportGraph graph{arena, interner};
auto &module = *graph.addModule("name");

module.declare("symbol", symbols::SymKind::Fn, visibility);
module.lookup("symbol");
module.lookupLocal("symbol");
module.lookupAll("symbol");
module.enterScope();
module.exitScope();
module.forEachLocal(callback);
module.forEachAll(callback);

graph.addDependency(moduleA, moduleB);
graph.finalize();
graph.isAncestor(parent, child);
graph.distance(parent, child);
graph.beginResolve(module);
```

Visibility is encoded by `SymbolVisibility` with `Public`, `Private`, or `Module`. Public symbols
are reachable from all modules; private symbols stay local; module-scoped symbols use
`ModuleVisibilityRange` ancestors/descendants limits through the import graph.

## Usage Flow

1. Create `Arena` and `StringInterner`.
2. Create modules with `addModule`.
3. Add dependencies and declare symbols with `Module::declare`.
4. Move into nested scopes when needed, then call `finalize`.
5. Query symbols with `lookup`, `lookupLocal`, `lookupAll`, `forEachLocal`, or `forEachAll`.
6. Use `beginResolve` for recursive resolution flows; the guard finishes resolution on destruction.

## Minimal Example

```cpp
Arena arena;
StringInterner strings{arena};
ImportGraph graph{arena, strings};

auto &app = *graph.addModule("app");
auto &lib = *graph.addModule("lib");
graph.addDependency(app, lib);

SymbolVisibility pub{};
pub.kind = SymbolVisibilityKind::Public;
lib.declare("run", SymKind::Fn, pub);
app.declare("main", SymKind::Fn, pub);

(void)graph.finalize();
app.forEachAll([](symbols::SymId id, const Module::SymbolData &data) {
    (void)id;
    (void)data;
});
```

## Dependencies

`zct_import` depends on `zct_common` and `zct_symbols`. It exposes the project source and build
include directories so consumers can include `common/import/import-graph.hpp`.

## Demo

`tests/import/demo.cpp` creates `vendor`, `service`, and `app` modules, adds dependencies,
declares symbols with public, module, and private visibility, and prints local and all-visible
symbols. It is registered as CTest `import-demo`:

```bash
cmake --build build --target import-demo -j
ctest --test-dir build -R '^import-demo$' --output-on-failure
```

## Agent Boundary

Do not modify `src/common/import/` or `src/symbols/` without explicit user approval. The demo and
its README consume only the existing public API and do not change the protected implementation.
