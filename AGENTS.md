# Repository Guidelines

## Project Structure & Module Organization

Zith is a C++23 compiler. Compiler subsystems live under `src/`; keep code in the
directory matching its namespace, such as `zith::parser` in `src/parser/`.
Important areas include `ast/`, `lexer/`, `parser/`, `symbols/`, `types/`,
`sema/`, `hir/`, `codegen/`, and `session/`. `src/session/compilation-session.*`
orchestrates the pipeline. Unit tests are standalone executables in `tests/`.
The language standard library is in `stdlib/`, examples in `examples/`, and
user-facing language documentation in `docs/`.

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
## Code Navigation & Semantic Search

This project is indexed by **OpenCodeRAG** (`opencode-rag`, alias `open-rad`).
It provides semantic code search, file skeletons, usage-finding, and image
description tools that agents MUST use before any code task.

### Setup (one-time)

```bash
# Initialise the workspace (already done — .opencode/ exists)
opencode-rag init

# Build the search index
opencode-rag index

# Verify the index is up to date
opencode-rag status
```

If `opencode-rag` is not yet installed globally:

```bash
npm install -g opencode-rag-plugin
opencode-rag setup   # registers the OpenCode plugin globally
```

### Mandatory tool-usage guidance

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

### Setup

`memsearch` is installed globally at `~/.local/bin/memsearch`.
Project-level config lives in `.memsearch.toml`.

```bash
# Index the project's markdown files (docs, AGENTS.md, etc.)
memsearch index . --max-chunk-size 1500

# Search indexed memory
memsearch search "query"

# Watch for changes and auto-reindex
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
