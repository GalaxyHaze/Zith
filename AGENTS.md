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

## Common Workflow

1. Read `readme.md` and the relevant rules file for the subsystem.
2. Change the declarative source or generator first.
3. Regenerate and build so the generated code reflects the rules.
4. Run the full test suite with `ctest --test-dir build --output-on-failure`.
5. Keep changes small enough that behavior and rollout can be reviewed incrementally.

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
