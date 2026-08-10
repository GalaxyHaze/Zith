# Project Config Helper

## Purpose

The project config helper generates the strongly typed `ProjectConfig` API from `default.toml`.

## Source Of Truth

Edit `default.toml` to change project configuration defaults. Regenerate the C++ side rather than
editing generated files.

Supported values are strings, integers, booleans, and arrays of strings.

## Regenerate

```bash
python3 src/config/project/generate.py \
  src/config/project/default.toml \
  --out build/src/config/project
```

## Verify

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Details about supported sections and field naming are in `src/config/README.md`.
