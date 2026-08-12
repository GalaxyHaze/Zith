# AGENTS.md

## Purpose

Zith `autonom` is an experimental tooling branch. The goal is to keep compiler and tooling code
simple to read and stable to maintain by encoding repetitive structure declaratively and
generating the boring C++ from compact source files.

Read `readme.md` before changing a subsystem. It documents the declarative helpers and the source
of truth for each area.

## Maintainability Principles

- Prefer changing a declarative rules file over hand-editing generated parser, lexer, or config
  code. Generators are not one-off scripts; they are the intended maintenance path.
- Do not modify generators (`src/*/generate.py`, `src/*/*/generate.py`,
  `src/config/project/scaffold.py`, `tools/rules_kit/`, or `tools/test_kit/`) unless the user
  explicitly approves a generator change. Running `scaffold.py` to create or refresh a new tree is
  allowed; changing the script itself is not.
  Most work is intended to be a `.rules`/TOML edit plus a handwritten action/handler update.
- Do not modify `src/symbols/` or `src/common/import/` without explicit user approval.
  The symbol generator and the import graph are a protected subsystem.
- Keep handwritten code focused on behavior, not plumbing. If a change is mostly table wiring or
  option parsing, it probably belongs in the generator and its rules file.
- Keep the app entry point intentionally thin. Do not move parsing, dispatch, project
  lifetime, or architectural logic into `src/app/zithc-main.cpp` until there is a concrete need.
- Add complexity incrementally and experimentally. Text or structure that is not yet used should
  stay out of the shipped surface.
- Prefer one clear mechanism over layering several abstractions for the same problem. The
  generators are the mechanism here, and the common runtime exists only for stable support types.
- Prefer the local common runtime over `std::` containers for internal state:
  `DynArray`, `Arena`, `FlatMap`, `Optional`, `Result`, and `StringInterner` are the intended
  pieces for ownership, storage, lookup, and optional/result-shaped values in handwritten code.
  `std::string_view`, `std::array` in generated tables, and `std::` only when the local facility
  does not apply are acceptable.
- Keep generated output untouched by hand edits. Regenerate it, and keep the generator as the
  reproducible source of the emitted files.
- Keep shared generator logic in `tools/rules_kit/` instead of duplicating it in sibling
  generators. Put generator tests in `tools/test_kit/` only through the existing helper surface.
- Treat a `.rules`/TOML file as the declarative surface: edit that file first, regenerate through
  the documented command, and verify generated C++ through the build and tests. If a requested
  change needs a new generator capability, stop and ask before modifying a generator.

## Common Workflow

1. Read `readme.md` and the relevant rules file for the subsystem.
2. Determine the normal edit surface:
   - `.rules` / TOML: change the declarative structure.
   - handwritten C++: implement behavior in the documented `actions.cpp`, `handlers.cpp`,
     `dispatch.cpp`, or types header.
   - generator/shared tooling: only change with explicit user approval; otherwise stop and ask.
   - protected subsystem: never touch `src/symbols/` or `src/common/import/` without explicit
     user approval; even read-only audits should note that the boundary remains unchanged.
3. Do not edit files under `build/`; they are generated outputs.
4. Regenerate and build so the generated code reflects the rules.
5. Run the full test suite with `ctest --test-dir build --output-on-failure`.
6. Keep changes small enough that behavior and rollout can be reviewed incrementally.

## Project Search

Investigate the code with the agent's standard tooling and confirm against `readme.md` and the
existing tests.

## Verification Notes

- `src/app/zithc-main.cpp`, generated artifacts, and handwritten handlers should compile with the
  C++23 project build.
  build.
- The test suite is the consent boundary for CLI behavior: help output, version output, valid
  dispatch, and typo suggestions are covered there.
- If a generator changes behavior, verify the generated result, not only the Python generator
  source.
- A generator change requires explicit user approval and must be verified by the generator
  regression tests plus `ctest --test-dir build --output-on-failure`.
- A `.rules`/TOML edit must keep the generated/source boundary unchanged: structure and table
  wiring belong in the declarative file, behavior belongs in handwritten C++. Verify the rebuilt
  generated files with the subsystem regression test and `ctest --test-dir build --output-on-failure`.
