# Import Demo

## Purpose

`tests/import/demo.cpp` exercises the protected `toolkit::import::ImportGraph` and the generated
symbol API. It creates `vendor`, `service`, and `app` modules, adds dependencies, finalizes the
graph, declares symbols with different visibility kinds, and prints local and visible symbols.

## Build And Run

The demo is registered as the CTest test `import-demo`:

```bash
cmake --build build --target import-demo -j
ctest --test-dir build -R '^import-demo$' --output-on-failure
```

## Agent Boundary

`src/common/import/` and `src/symbols/` are protected. The demo consumes the existing public API
and does not modify or bypass the protected implementation. Do not change the import graph or
symbol generator without explicit user approval.
