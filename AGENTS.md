#Repository Guidelines

## Project Structure And Module Organization

Zith is a C++23 compiler. Active compiler subsystems live under `src/` and
follow the current module layout: `memory/`, `support/`, `common/`, `cli/`,
`session/`, `diagnostics/`, `cache/`, `capi/`, `codegen/`, `comptime/`,
`formatter/`, `hir/`, `sema/`, `symbols/`, `types/`, `wasm/`, `zirl/`,
`frontend/`, `cinterop/`, `cc/`, `ide/`, `interp/`, and `vm/`. The `frontend/`
module owns lexing, parsing, AST lowering, macros, and frontend printing.
Binary library format code lives in `zirl/`. `src/session/compilation-session.*`
orchestrates the pipeline. Unit tests are standalone executables in `tests/`.
The language standard library is in `stdlib/`, examples are in `examples/`,
and user-facing language documentation is in `docs/`.

## Zith-- Source of Truth

The `main` compiler targets `Zith--`, a sibling language to `Zith`. It does not
compile a subset of Zith or represent a development stage on the way to Zith.
The canonical `Zith--` specification is `docs/Zith--.md`; the separate Zith
design specification is `docs/Zith-spec.md`. Compiler feature status applies
only to `Zith--` and is tracked in `docs/impl-status.md`.

The implementation rules for the `Zith--` frontend, sema, HIR, codegen and tests
are in `docs/Zith---implementation.md`. Changes to bindings, storage, const
semantics, ownership or macro restrictions must stay consistent with that
document and `docs/Zith--.md`, and update them when behavior changes.

## Build, Test, and Development Commands

Configure and build from the repository root:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Use `./build/zithc --help` to inspect the local CLI. A useful compiler smoke
test is `./build/zithc check examples/loops-simple.zith` (run from the repo
root, so project discovery stops at the checkout).
LLVM remains optional for parser/sema-only local builds, but every native
release artifact and the native CI jobs must configure `ZITH_REQUIRE_LLVM=ON`
with LLVM 18+; those release paths fail instead of publishing a sema-only
compiler.
Use `cmake --build build --target fmt` to format sources, or
`cmake --build build --target fmt-check` to verify formatting without edits.

The format and `fmt-check` targets are created only when `clang-format` is found
on `PATH`;
if a local LLVM has a different version, either put `clang-format<N>`
on `PATH` or re-run CMake after installing it.

### Run everything through `agent-run`

Always wrap commands you execute with the `agent-run` helper in `~/.local/bin`
(already on `PATH`). It applies a wall-clock timeout, `nice`, `ionice`, and an
optional per-process CPU cap so a hung or runaway process (a compiler loop, a
test, a build) cannot cook the CPU or SSD:

```bash
# Builds: cap CPU off, keep the timeout generous.
agent-run -t 900 -c 0 -- cmake --build build -j4

# Probes and test runs: a tight timeout, a small CPU cap.
agent-run -t 30 -c 50 -- ./build/zithc --no-cache run prog.zith

# Tests.
agent-run -t 900 -c 0 -- ctest --test-dir build --output-on-failure
```

Use `-t <secs>` for the timeout (`0` disables it), `-c <pct>` for the CPU cap
(`0` disables it), and `-h` for the full option list. The helper exits `124`
on timeout. This matters for `zithc`: malformed input has historically made the
LLVM backend loop at 100% CPU, so an unwrapped `zithc` invocation can hang the
machine.

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
option(ZITH_ENABLE_C_COMPILE "Enable recursive .c compilation for ffi.c_source_dirs / --c-source-dir" ON)
option(ZITH_TCC_FETCH "Fetch and build TinyCC when libtcc is not found on the system" ON)
option(ZITH_ENABLE_DEBUG_PRINT "Enable compile-time debug prints in compiler internals" OFF)
option(ZITH_ENABLE_C_INTEROP "Enable automatic C header bindings through libclang" ON)
```

`ZITH_HAS_LLVM` is auto-disabled when LLVM 18+ is not found, not just when
`-DZITH_HAS_LLVM=OFF` is passed. `ZITH_ENABLE_C_COMPILE` and
`ZITH_ENABLE_C_INTEROP` are disabled for WebAssembly and cross-compilation.

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

Modern compiler source lives under `src/`; the archived legacy parser/sema
library is outside the active tree and no longer participates in the build.

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
Source → Lex → Scan → Import → Resolve → Sema → Comptime/Solve → SRA → HIR → Codegen → Cache
```

`CompilationSession` in `src/session/compilation-session.*` orchestrates these
stages. `src/session/frontend-context.*` prepares parser/frontend state, and
`src/session/pipeline-plan.*` records the planned stages for a session.
`GenericInstantiationPass` in `src/comptime/generic-instantiate.*` runs between
`semaStage()` and `sraStage()`, monomorphizing generic functions, structs,
aliases, methods, and `implement` blocks.

When reading or modifying a compiler subsystem, start with the corresponding
`memory/<area>-*.md` file when one exists, then `CompilationSession` to see how
the subsystem is invoked.

## Testing Details

Tests are standalone executables under `tests/` and are registered through the
`add_zith_test` helper in the root `CMakeLists.txt`. A few notes:

- Tests build against `zithcLib`; no legacy archive is linked.
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
and `build: ...`. Keep each commit narrowly scoped.

Make **small commits** during the work, not one large commit at the end. Commit
each self-contained step as soon as it builds and its focused tests pass (a fix,
its regression test, and its docs are three reasonable commits, or one small
commit when they are truly inseparable). A dirty worktree left uncommitted for a
long session is hard to review and easy to lose, so prefer committing early and
often over batching everything into a final commit.

Pull requests should explain the compiler behavior changed, list tests run, link
relevant issues, and include sample source or output when diagnostics, syntax,
or CLI behavior changes.

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

## Local RAG Fallback (sandbox-safe)

OpenCodeRAG needs Ollama for query embeddings and is not available in every
sandbox. For local code/docs lookup use the daemon-free tool instead:

```bash
python3 scripts/rag.py index
python3 scripts/rag.py search "<query>" --kind code --json
python3 scripts/rag.py search "<query>" --kind docs --json
python3 scripts/rag.py show <chunk_id> --kind code
```

The tool builds `.rag-index/` with separate code and docs corpora, uses BM25
with optional llama.cpp embeddings, and exposes the same functionality through
`scripts/rag-mcp.py`. See `docs/rag.md` for details.

Outside the sandbox, register the MCP server once with ByteAsk:

```bash
byteask mcp add rag \
  --env RAG_PROJECT_DIR=/home/diogo/Zith \
  --env RAG_INDEX_DIR=/home/diogo/Zith/.rag-index \
  -- python3 /home/diogo/Zith/scripts/rag-mcp.py
```

Verify it appears in the client:

```bash
byteask mcp list
byteask mcp get rag
```

## Agent skills

### Issue tracker

Issues and specs are tracked in GitHub Issues. See `docs/agents/issue-tracker.md`.

### Triage labels

Use the default triage label vocabulary. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context domain docs layout. See `docs/agents/domain.md`.

### Writing style

Three genres cover the project's prose: spec, docs, and log. Use the genre
definitions and shared rules (no emojis, no em dashes, no semicolon as
period) in `docs/writing-style.md` when writing or reviewing any of them.

### Sequenta

Before starting any work that is a linear sequence of agent task steps, ask the user whether to use Sequenta. If the user approves, read `/home/diogo/.byteask/skills/sequentia/SKILL.md` and use `/home/diogo/.byteask/skills/sequentia/scripts/sequenta` for state transitions and verification. Do not create Sequenta module files or `.sequenta` state without user approval.

## Memory & Knowledge Management (memsearch)

This project also uses **memsearch** for semantic memory search across
markdown knowledge bases (docs, notes, project memory).

### Project Memory Files

Persist useful discoveries about the project as Markdown files under `memory/`,
one file per topic or area. Keep each note as short as useful. Split a note
when distinct topics need independent retrieval or when a real playbook makes
the file unwieldy, then link the resulting notes from `memory/README.md`.

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
