# Config Helper

## Purpose

The config subsystem generates strongly typed project configuration from `default.toml`. It
removes the repetitive C++ required to expose configuration defaults and typed fields.

The source of truth is the TOML file, not the generated C++.

## Files

| File | Responsibility |
|---|---|
| `flags/default.toml` | Declarative project configuration schema and defaults. |
| `project/generate.py` | TOML parser and C++ generator. |
| `project/CMakeLists.txt` | CMake wiring for the generated config target. |
| `build/src/config/project/project-config.hpp` | Generated public configuration API. |
| `build/src/config/project/project-config.cpp` | Generated parsing/loading implementation. |

## Rules Syntax

The TOML file uses sections and key/value entries:

```toml
[project]
name = "turv"
version = "0.1.0"

[build]
entry = "src/main.turv"
opt_level = 0
verbose = false
lto = false
color = "auto"

[paths]
src_dir = "src"
doc_dir = "docs"

[ffi]
include_dirs = ["include", "src"]
```

Supported scalar types:

| TOML form | Generated C++ type |
|---|---|
| string | `common::memory::InternedId` |
| integer | `int` |
| boolean | `bool` |
| array of strings | `common::memory::DynArray<common::memory::InternedId>` |

Field names are converted to camelCase in generated C++.

## Common Workflow

1. Read the current `default.toml` before changing configuration.
2. Edit `src/config/flags/default.toml`.
3. Regenerate:

```bash
python3 src/config/project/generate.py \
  src/config/flags/default.toml \
  --out build/src/config/project
```

4. Rebuild and run the test suite:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The normal project build regenerates the config automatically.

## Validation

The generator rejects:

- unknown TOML sections
- repeated fields inside a section
- arrays that contain values other than strings
- malformed strings, booleans, and integers

Keep generated config code free of hand edits. If a shape changes, change the TOML schema and
regenerate.

## Agent Boundary

Normal config changes belong in `src/config/flags/default.toml`. Do not edit
`build/src/config/project/*`; those files are rebuilt from the TOML. Do not modify
`project/generate.py` or shared generator rules without explicit user approval.

## Public API

The generated surface lives in `toolkit` and exposes:

- `ProjectConfig` with interned string fields, integer/bool fields, and a `DynArray` for
  string list fields.
- `loadFromToml(std::string_view toml, Arena &arena, StringInterner &strings,
  ProjectConfig &out)` returning `Result<void, Error>`.

String values are returned as `InternedId`; pass them back to the same `StringInterner` to get
`std::string_view`. The `StringInterner` must outlive config lookup calls because the generated
config stores string values as interned IDs.

## Demo

`tests/config/config-demo.cpp` loads a small TOML containing `[project]`, `[build]`, `[paths]`,
and `[ffi]`, then prints the loaded values through `strings.lookup`.

```bash
cmake --build build --target config-demo -j
ctest --test-dir build -R '^config-demo$' --output-on-failure
```
