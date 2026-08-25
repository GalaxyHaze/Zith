#Repository Guidelines

##Project Structure &Module Organization

        Zith is a C++ 23 compiler.Compiler subsystems live under `src /`; keep code in the
directory matching its namespace, such as `zith::parser` in `src/parser/`.
Important areas include `ast/`, `lexer/`, `parser/`, `symbols/`, `types/`,
`sema/`, `hir/`, `codegen/`, and `session/`. `src/session/compilation-session.*`
orchestrates the pipeline. Unit tests are standalone executables in `tests/`.
The language standard library is in `stdlib/`, examples in `examples/`, and
user-facing language documentation in `docs/`.

## Zith-- Source of Truth

The `main` compiler builds the `Zith--` subset of the language. The canonical
user-facing specification is `docs/Zith--.md`, and the implementation rules for
frontend, sema, HIR, codegen and tests are in `docs/Zith---implementation.md`.
Changes to bindings, storage, const semantics, ownership or macro restrictions
must stay consistent with both files and update them when behavior changes.

## Build, Test, and Development Commands

Configure and build from the repository root:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Use `./build/zithc --help` to inspect the local CLI. A useful compiler smoke
test is `./build/zithc --include stdlib check examples/hello-world.zith`.
LLVM is optional: CMake disables native code generation when it is unavailable.
Use `cmake --build build --target fmt` to format sources, or
`cmake --build build --target fmt-check` to verify formatting without edits.

The format and `fmt-check` targets are created only when `clang-format` is found
on `PATH`;
if a local LLVM has a different version, either put `clang-format<N>`
on `PATH` or re-run CMake after installing it.

## CMake Options & Feature Gate

The root `CMakeLists.txt` exposes several options that change which compiler
subsystems are built. Configure with
`cmake -S . -B build -D<OPTION>=ON/OFF` as needed:

```cmake
option(ZITH_BUILD_CLI "Build the zithc CLI executable" ON)
option(ZITH_BUILD_CAPI "Build the C ABI library" ON)
option(ZITH_BUILD_BENCHMARKS "Build local benchmark executables" OFF)
option(ZITH_IS_WASM "Build for WebAssembly target" OFF)
option(ZITH_HAS_LLVM "Enable LLVM codegen backend" ON)
option(ZITH_BUILD_LEGACY_LIB "Build the legacy parser/sema compatibility library" ON)
option(ZITH_ENABLE_C_COMPILE "Enable recursive .c compilation for ffi.c_source_dirs / --c-source-dir" ON)
option(ZITH_TCC_FETCH "Fetch and build TinyCC when libtcc is not found on the system" ON)
option(ZITH_ENABLE_DEBUG_PRINT "Enable compile-time debug prints in compiler internals" OFF)
option(ZITH_ENABLE_C_INTEROP "Enable automatic C header bindings through libclang" ON)
```

`ZITH_HAS_LLVM` is auto-disabled when LLVM 18+ is not found, not just when
`-DZITH_HAS_LLVM=OFF` is passed. `ZITH_ENABLE_C_COMPILE` and
`ZITH_ENABLE_C_INTEROP` are disabled for WebAssembly and cross-compilation.
`ZITH_BUILD_BENCHMARKS` requires `ZITH_BUILD_LEGACY_LIB=ON`.

## Debug Prints & Temporary Instrumentation

Do not add raw `std::printf`, `std::cerr`, or `fprintf(stderr, ...)` debug output
that later has to be removed again. Instead use the project debug print helper:

```cpp
#include "support/debug-print.hpp"

ZITH_DEBUG_PRINT("lowering function %s\n", name.c_str());
```

    Temporary instrumentation must keep a small footprint in the final diff.
`ZITH_DEBUG_PRINT(...)` evaluates like a void expression when disabled,
    so it can be left in place without affecting release builds.Build with it enabled when
        diagnosing a subsystem :

```bash cmake
        - S.- B build - debug
        - DZITH_ENABLE_DEBUG_PRINT = ON cmake-- build build - debug -
                                     j
```

                                     Print to `stderr` only,
          and use the `[zith - debug]` prefix already provided by the helper so compiler output does
              not mix with child program stdout.Remove
              instrumentation once the diagnosed code is fixed;
keep only the helper itself.

    ##Project Layout Details

        Modern compiler source lives under `src /`;
`lib/legacy-zith/` contains the
legacy parser/sema compatibility library that the modern pipeline still links
against. The CMake target `legacy-zith` and `zithcLib` reference each other, so
tests link both with `-Wl,--start-group ... -Wl,--end-group` to allow circular
archive resolution.

Common language/codebase organization:

- `src/memory/` — arena, `DynArray`, `FlatMap`, `Optional`, `Result`, source files.
- `src/frontend/` — lexing/parsing glue, frontend printer, macro expansion.
- `src/symbols/` — imports, symbol tables, resolver.
- `src/types/` — type intern, lowering, unification.
- `src/sema/` — semantic analysis, typed AST, modern HIR lowering, NRA facts.
- `src/comptime/` — generic instantiation, solver, compile-time evaluation.
- `src/hir/` — high-level IR module and HIR verification.
- `src/codegen/` — LLVM IR emission; removed from the build without LLVM.
- `src/session/` — `CompilationSession` pipeline orchestration.
- `src/cache/` — incremental compilation and artifact storage.
- `src/capi/` — C ABI wrapper (`zithcLib`).
- `src/cli/` — CLI entry point and command dispatch.
- `src/zirl/` — Zith IR library format sections.
- `src/cc/`, `src/cinterop/` — companion C compilation and C header bindings.
- `src/wasm/` — WebAssembly playground and WASI stubs.

## Pipeline And Compiler Entry Points

The current documented pipeline is:

```
Source → Lex → Scan → Import → Resolve → Sema → Comptime/Solve → NTA/NRA → HIR → Codegen → Cache
```

`CompilationSession` in `src/session/compilation-session.*` orchestrates these
stages. `src/session/frontend-context.*` prepares parser/frontend state, and
`src/session/pipeline-plan.*` records the planned stages for a session.
`GenericInstantiationPass` in `src/comptime/generic-instantiate.*` runs between
`semaStage()` and `nraStage()`, monomorphizing generic functions, structs,
aliases, methods, and `implement` blocks.

When reading or modifying a compiler subsystem, start with the corresponding
`memory/<area>-*.md` file when one exists, then `CompilationSession` to see how
the subsystem is invoked.

## Testing Details

Tests are standalone executables under `tests/` and are registered through the
`add_zith_test` helper in the root `CMakeLists.txt`. A few notes:

- Tests build against both `zithcLib` and `legacy-zith` when legacy support is
  enabled, so expect archive-ordering issues if that linkage changes.
- The example suite (`test-examples`) drives the real CLI and requires
  `ZITH_HAS_LLVM` plus the `zithc` target.
- Benchmarks are opt-in via `ZITH_BUILD_BENCHMARKS=ON`; lexer smoke tests are
  registered as CTest cases only when the bench target exists.
- The repository builds with `-Werror` under Clang and uses `-Weverything`
  with a curated set of suppressions. New code must satisfy that warning set.

Useful focused commands:

```bash
cmake --build build --target test-sema -j 4
./build/tests/test-sema
ctest --test-dir build -R 'lexer' --output-on-failure
```

## Coding Style & Naming Conventions

Follow `.clang-format`: four spaces, no tabs, 100-column limit, attached braces,
and right-aligned pointers. Use `clang-format` rather than manually reformatting
unrelated code. Names use existing C++ conventions: classes and structs in
`PascalCase`, functions and fields in `camelCase`/`snake_case` as established by
the local module. Preserve arena-backed ownership patterns (`memory::Arena`,
`DynArray`) and do not introduce exceptions or RTTI.

## Testing Guidelines

Add focused coverage in `tests/test-<area>.cpp` and register the executable in
the root `CMakeLists.txt` through `add_zith_test`. Keep tests deterministic and
exercise both accepted input and diagnostics when changing parsing, imports, or
semantic analysis. Run the full CTest suite before submitting.

## Commit & Pull Request Guidelines

Recent history uses concise conventional prefixes when useful, for example
`feat: parser support for extern fn declarations`, `fix: ...`, `refactor: ...`,
and `build: ...`. Keep each commit narrowly scoped. Pull requests should explain
the compiler behavior changed, list tests run, link relevant issues, and include
sample source or output when diagnostics, syntax, or CLI behavior changes.

<!-- BEGIN opencode-rag -->
## Code Navigation

ALWAYS use OpenCodeRAG tools before reading or editing:
- **Search first** — `search_semantic(query)` instead of grep/glob
- **Skeleton before read** — `get_file_skeleton(filePath)` then read specific lines
- **Usages before edit** — `find_usages(symbolName)` before modifying any symbol
- **Images via describe** — `describe_image(filePath)` — never read raw bytes
- **Recall quirks** — `recall_quirks(query)` when you hit a known pitfall
- **Add quirks** — `add_quirk(content)` when you discover a non-obvious fact
- **Fix quirks** — `update_quirk(id, ...)` / `delete_quirk(id)` when a stored quirk is outdated or wrong

If no results, run `opencode-rag index`.

### Decision tree — ALWAYS follow this order
1. User mentions code behavior/architecture → `search_semantic(query)`
2. User mentions a file path → `get_file_skeleton(filePath)` THEN `read` on specific lines
3. User mentions a function/class/variable to edit → `find_usages(symbolName)` THEN `search_semantic` THEN `edit`
4. User asks a code question → `search_semantic` to gather context before answering
5. User asks about an image or visual asset → `describe_image(filePath)` to retrieve its generated description, then optionally `search_semantic` for related code
6. You encounter an error or need to recall a known pitfall → `recall_quirks(query)`
7. You discover a non-obvious fact or workaround → `add_quirk(content)` to persist it for future sessions
8. A recalled quirk is outdated or wrong → `update_quirk(id, ...)` to fix it, or `delete_quirk(id)` if it no longer applies

### Proactive triggers — you MUST call these tools when
- User asks about code behavior, architecture, or implementation details
- User asks to edit, refactor, or fix code — call `find_usages` first
- User references files or functions you haven't read yet
- User says "find", "search", "look up", "where is", "how does"
- User refers to an image, screenshot, diagram, or visual asset
- Before answering ANY code-related question, retrieve context first
- Before reading ANY file, call `get_file_skeleton` to orient first

### Anti-patterns — NEVER do these
- Reading full files without calling `get_file_skeleton` first (wastes tokens)
- Editing a function without calling `find_usages` first (breaks call sites)
- Answering code questions without calling `search_semantic` first (you guess at behavior)
- Using `grep`/`glob` when `search_semantic` would find the answer faster
- Treating image files as text — use `describe_image` instead of reading raw bytes
- Using `npx opencode-rag quirk` shell commands instead of the built-in quirk tools (`add_quirk` / `recall_quirks` / `update_quirk` / `delete_quirk`) (the tools are faster, already loaded in-process, and go through the trust monitor)

### MANDATORY quirk capture rules — you MUST call `add_quirk` when
- A build, test, or type-check command fails and you resolve it
- You discover an undocumented library constraint, peer dep, or workaround
- You learn an environment-specific requirement (OS, tool version, etc.)
- You make a design decision that future sessions should remember
- You resolve a gotcha that cost more than one attempt

### MANDATORY quirk hygiene — you MUST call `update_quirk` or `delete_quirk` when
- A stored quirk is outdated, wrong, or has been fixed — update it or delete it instead of adding a contradicting duplicate
- NEVER finish a coding session without adding quirks for resolved errors.
<!-- END opencode-rag -->

## Memory & Knowledge Management (memsearch)

This project also uses **memsearch** for semantic memory search across
markdown knowledge bases (docs, notes, project memory).

### Project Memory Files

Persist useful discoveries about the project as Markdown files under `memory/`,
one file per topic or area. Keep every file between 200 and 300 lines;
if a
topic grows beyond 300 lines, split it into more focused files and link them
from `memory/README.md`.

Use these files as a lightweight durable memory layer, independent of code
search indexes:
- Save non-obvious facts, design decisions, conventions, workarounds, build
  gotchas, and architecture notes discovered while working.
- Consult the relevant file before answering code questions or editing that
  area, whenever the memory entry could affect the work.
- Update the file when the project behavior changes, not just when new facts
  appear.
- Invalidate or rewrite an entry when it becomes outdated, wrong, or misleading
  instead of accumulating contradictory notes.

File naming convention: `memory/<area>-<topic>.md` (for example
`memory/build-cmake.md`). Start each file with a short title and one-paragraph
summary, then use concise sections. Keep facts tied to the current state of the
repository and include enough context to decide whether the note is still valid.

### Setup

`memsearch` is installed globally at `~/.local/bin/memsearch`.
Project-level config lives in `.memsearch.toml`.

```bash
#Index the project's markdown files (docs, AGENTS.md, etc.)
memsearch index . --max-chunk-size 1500

#Search indexed memory
memsearch search "query"

#Watch for changes and auto - reindex
memsearch watch .
```

### When to use memsearch
- Searching project documentation, changelogs, and design notes
- Recalling past decisions, conventions, or gotchas recorded in markdown
- Finding relevant context from `docs/`, `AGENTS.md`, or any `.md` file
- Complementing `opencode-rag` (code-focused) with documentation-focused search

### When to use opencode-rag (open-rad) instead
- Searching source code, AST structures, or function signatures
- Finding usages of symbols, types, or variables
- Getting file skeletons or structural overviews
- Describing images or screenshots

### Combined workflow
1. `memsearch search` for documentation/knowledge questions
2. `search_semantic` (opencode-rag) for code questions
3. `recall_quirks` for experiential memory from past sessions
