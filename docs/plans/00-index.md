# Zith Autonom --- Foundational Reorganization Plan

## Summary

This plan reorganizes the Zith Autonom branch around three principles:
the `common/` runtime is the single C++ foundation for all helpers and generators;
shared Python tooling eliminates duplication across generators and test suites;
and the parser, AST, and diagnostic subsystems gain template-based helpers that
work with any user-provided types without coupling them together.

The plan is split into six steps with a strict dependency graph. Steps 1 and 2
are prerequisites for all others and must run sequentially (not in parallel)
because they touch the same generator files. Steps 3-6 can then run in parallel
with one caveat: Steps 5 and 6 both modify `tests/common/CMakeLists.txt` and
need a trivial merge.

## Dependency Graph

```
Step 1 (Common C++ Runtime)     <-- physical directory moves + renames
  |
  v
Step 2 (Python Rules Kit)       <-- same generator files as Step 1, must follow
  |
  +-----> Step 3 (Test Harness)      <-- only touches test *.py files
  |
  +-----> Step 4 (Parser Refactor)   <-- parser subsystem, isolated
  |
  +-----> Step 5 (AST Helpers)       <-- new common/ast/, tests/common/
  |         |
  |         v conflict on tests/common/CMakeLists.txt (trivial merge)
  +-----> Step 6 (Diagnostic Render) <-- new common/diagnostic/, tests/common/
```

## Common Directory Layout After All Steps

```
src/common/
  CMakeLists.txt
  README.md
  memory/                         namespace common::memory
    arena.hpp / arena.cpp
    dyn-array.hpp
    flat-map.hpp
    optional.hpp
    result.hpp
    source-file.hpp / source-file.cpp
    source-map.hpp / source-map.cpp
    span.hpp
    string-interner.hpp / string-interner.cpp
  diagnostic/                     namespace common::diagnostic
    diagnostic.hpp
    levenshtein.cpp
    render.hpp / render.cpp       (Step 6)
  text/                            namespace common::text
    parse.hpp / parse.cpp
  ast/                             namespace common::ast  (Step 5)
    concepts.hpp
    visit.hpp
    clone.hpp
    replace.hpp
    prune.hpp
    transform.hpp
  parser/                          namespace common::parser  (Step 4)
    builder.hpp
```

## Step Map

| Step | Title | Prerequisites | Runs in parallel with |
|------|-------|---------------|----------------------|
| 1 | Common C++ Runtime | None | Nothing (touches generators) |
| 2 | Python Rules Kit | Step 1 | Nothing (touches same generators) |
| 3 | Test Harness | Step 2 | 4, 5, 6 |
| 4 | Parser Template Refactor | Steps 1, 2 | 3, 5, 6 |
| 5 | AST Builder Helpers | Steps 1, 2 | 3, 4, 6 (see note) |
| 6 | Diagnostic Renderer | Steps 1, 2 | 3, 4, 5 (see note) |

**Note on Steps 5 and 6:** Both add a test target to `tests/common/CMakeLists.txt`.
These edits are adjacent but not overlapping. If run in parallel, one branch
will have a trivial merge conflict in that file. The safe approach is to run
5 and 6 sequentially, or have one branch add both targets and the other rebase.

## File Collision Map

| File | Touched by steps | Risk |
|------|-----------------|------|
| `src/cli/generate.py` | 1, 2 | High: must be sequential |
| `src/config/project/generate.py` | 1, 2 | High |
| `src/frontend/lexer/generate.py` | 1, 2 | High |
| `src/frontend/ast/generate.py` | 1, 2 | High |
| `src/frontend/parser/generate.py` | 1, 2, 4 | High: 4 must follow 1+2 |
| `src/session/generate.py` | 1, 2 | High |
| `tests/frontend/CMakeLists.txt` | 3, 4 | Low: different sections |
| `tests/common/CMakeLists.txt` | 5, 6 | Medium: same section, different targets |
| `src/common/CMakeLists.txt` | 1, 5, 6 | Low: 1 rewrites paths, 5+6 append |

## Recommended Execution Order

1. Step 1 (single branch, physical file moves + renames)
2. Step 2 (single branch, after Step 1 merges)
3. Steps 3, 4, 5, 6 can start in parallel branches after Step 2 merges.
   Resolve the `tests/common/CMakeLists.txt` conflict between 5 and 6 by
   having one of them land first.

## Acceptance Criteria

All steps are complete when:

- `cmake --build build -j && ctest --test-dir build --output-on-failure` passes
  with zero regressions.
- Every generator `generate.py` imports from `tools.rules_kit` and contains no
  local copy of `strip_comment`, `cpp_string`, `split_top_level`, or `RuleError`.
- Every regression test imports from `tools.test_kit`.
- `Parser<Output>` no longer duplicates `TokenStream` methods; hooks access
  `TokenStream` via `parser.tokenStream()`.
- `common/diagnostic/`, `common/text/`, `common/ast/`, and `common/parser/` headers
  compile standalone and have dedicated test executables.
- `memory::` is now `common::memory::` with files physically under `common/memory/`.

## Conventions

- File links in each step file use absolute paths rooted at the repo root.
- "No regression" means the existing test suite passes unmodified except for
  mechanical renames and import path changes.
- Each step lists its own test plan; the index does not repeat them.
