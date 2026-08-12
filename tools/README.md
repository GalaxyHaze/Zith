# Shared Generator Tooling

`tools/` owns the Python behavior shared by the subsystem generators and by the generator
regression tests. It is protected code: do not modify it without explicit user approval.

## Layout

| Directory | Purpose |
|---|---|
| `tools/rules_kit/` | Shared rule parsing, generation output, error rendering, and text/identifier helpers. |
| `tools/test_kit/` | Shared generator-regression harness: rule-file creation, generator invocation, assertions, and smoke compilation. |

Subsystem generators import `tools.rules_kit`. Their regression tests import `tools.test_kit`.
Keep shared Python logic here instead of copy-pasting it into individual `generate.py` files.

## Usage

The rules file for each subsystem documents its own sections and regeneration command. The usual
manual drive is:

```bash
python3 src/<subsystem>/generate.py src/<subsystem>/<subsystem>.rules --out build/src/<subsystem>
```

For generators that accept a user-owned types header, pass `--types src/<subsystem>/types.hpp`.

Verify rules or generator behavior with:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run only the generator regression tests with:

```bash
ctest --test-dir build -R generator --output-on-failure
```

## Boundaries

- Do not edit generated files under `build/`.
- Do not edit `tools/rules_kit/` or `tools/test_kit/` without explicit user approval.
- Normal work is declarative-first: change `.rules`/TOML, update handwritten behavior, rebuild,
  and verify through the test suite.
- If a requested change needs a new generator capability or shared helper, stop and ask before
  modifying the protected tooling.
