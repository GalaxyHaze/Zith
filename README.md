# Zith Compiler's Toolkit

> **Status: Experimental tooling repository.**
> Zith Compiler's Toolkit (ZCT) is a declarative compiler toolchain. The default showcase is
> Zith, a deliberately small language; the toolchain exists to show how repetitive compiler
> and tooling code can be kept small through generators and declarative inputs.

A large part of compiler and tooling code is repetitive: command-line parsing, token tables,
configuration defaults, typed config loading, and error-prone glue code. ZCT moves those
repetitive parts into compact source descriptions and lets
small helpers generate the C++ implementation.

The result is a repository that aims to be:

- simpler to extend
- easier to review
- cheaper to refactor
- more consistent across modules
- less dependent on copy/paste patterns

---

## What This Repository Contains

The toolkit today includes several concrete helpers, the default Zith grammar used
by demos, and an experimental VM backend for the planned codegen roadmap:

| Helper | Purpose | Source of truth |
|---|---|---|
| CLI helper | Generate command parsing, dispatch, help text, and suggestions | `src/cli/README.md` |
| Lexer helper | Generate lexer tables and tokenization support code | `src/frontend/lexer/README.md` |
| AST helper | Generate AST node structs, allocation, walking, and printing | `src/frontend/ast/README.md` |
| Parser helper | Generate a token-driven parser surface with context rules and hooks | `src/frontend/parser/README.md` |
| Project-config helper | Generate strongly typed defaults and TOML loading code | `src/config/project/README.md` |
| Session helper | Generate the compilation pipeline and action hook surface | `src/session/README.md` |
| Diagnostic helper | Generate the error catalogue consumed by the common renderer | `src/diagnostic/README.md` |
| Symbol helper | Generate symbol kinds, symbol data layout, and basic helpers | `src/symbols/symbols.rules` |

The default Zith showcase grammar covers functions (`fn`), variables (`let`), `print`, boolean
literals, `if`/`else`, `while`, `return`, integer/float/string literals, arithmetic and
comparison operators, brackets, and `//` and `/* */` comments. The current demos exercise the
lexer, session pipeline, cache configuration, and the scratch VM without promising a complete
production language implementation.

Supporting those helpers is a small common runtime with arena allocation, interned strings,
results, option-like types, dynamic arrays, AST transforms, import graph APIs, SIR, and source
mapping under `src/common`.

The Python side stays deliberately thin. `tools/rules_kit/` provides the shared parser, output,
and entry-point primitives used by all generators; subsystem generators remain small
declarative-to-C++ translators. `tools/test_kit/` owns the shared generator-regression harness.
Generated files in `build/` are not source and must never be edited by hand.

`tools/` is the stable core of the tooling: shared Python logic that every generator and
generator regression test relies on. It is protected code and should only change with explicit
approval.

### Declarative Development Contract

Contributors should treat this repository as **declarative-first**:

- Normal changes edit a `.rules`/TOML file and, when behavior is needed, the documented
  handwritten `actions.cpp`, `handlers.cpp`, `dispatch.cpp`, or types header.
- Generated files under `build/` (`*generated*`, `cli.*`, `lexer.*`, `ast.*`, `parser.*`,
  `session.*`, `symbols.*`, `project-config.*`, `error-info.*`) are build output. Do not edit them.
- Do not change `src/*/generate.py`, `src/*/*/generate.py`,
  `src/config/project/scaffold.py`, `tools/rules_kit/`, or `tools/test_kit/` without explicit
  user approval. Running `scaffold.py` to create or refresh a project tree is allowed; editing
  the scaffold script itself is not. If a requested change needs a generator or shared-tooling
  edit, stop and ask before modifying it.
- If a constant/rule/table shape is missing, first look for a `.rules` or TOML declaration.
  Add ad-hoc logic to a generator only if the user explicitly approves a new generator
  capability; otherwise keep behavior in handwritten C++.
- Never modify `src/symbols/` or `src/common/import/` without explicit user approval.
  The symbol generator and the handwritten import graph are a protected area of the codebase.
- Edit a `.rules`/TOML file rather than extending a generator to make an existing declarative
  surface fit. Unknown sections and malformed declarations are rejected by generator validation;
  read the subsystem README before changing the grammar of the rules file.

Every rule remains supported by its own README, which documents the exact sections, hooks, and
regeneration command. When editing a rules file, keep the generated/source boundary unchanged:
structure and table wiring belong in rules, behavior belongs in C++.

### Managing `.rules` Files

Normal diagnostics should be able to start from this table when the source of a change is a
declarative file:

| Declarative source | Subsystem README | Generated output |
|---|---|---|
| `src/cli/cli.rules` | `src/cli/README.md` | `build/src/cli/*` |
| `src/frontend/lexer/lexer.rules` | `src/frontend/lexer/README.md` | `build/src/frontend/lexer/*` |
| `src/frontend/ast/ast.rules` | `src/frontend/ast/README.md` | `build/src/frontend/ast/*` |
| `src/frontend/parser/parser.rules` | `src/frontend/parser/README.md` | `build/src/frontend/parser/*` |
| `src/session/session.rules` | `src/session/README.md` | `build/src/session/*` |
| `src/config/flags/default.toml` | `src/config/README.md` | `build/src/config/project/*` |
| `src/diagnostic/error.rules` | `src/diagnostic/README.md` | `build/src/diagnostic/*` |
| `src/symbols/symbols.rules` | protected: ask before changing | `build/src/symbols/*` |

The normal rules-edit workflow is:

1. Read the subsystem README and the current rules file before editing.
2. Edit only the declarative `.rules` or TOML file; do not edit generated files in `build/`.
3. Add behavior to the documented handwritten C++ surface when the structure already exists.
4. Regenerate with the manual command from the README or rebuild the project.
5. Run the generator regression tests and the full test suite:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Generators are intentionally strict: they reject unknown sections, repeated entries, missing
required fields, invalid identifiers/types, and unknown hooks. If a rules change needs a new
declaration shape, do not silently extend `generate.py`; stop and request explicit permission to
change generator or shared-tooling behavior.

### Shared Tooling

`tools/rules_kit/` is the intended home for Python logic shared across subsystem generators:

| File | Owns |
|---|---|
| `tools/rules_kit/errors.py` | `RuleError` and rule-file error rendering. |
| `tools/rules_kit/text.py` | comments, logical lines, quoted/list parsing, typed members, hooks, identifiers, C++ quoting, naming. |
| `tools/rules_kit/runtime.py` | generated-file writing and `.gitignore` output. |

`tools/test_kit/` owns generator invocation, temporary rules creation, error assertions, and
smoke compilation used by the generator regression tests.

Keep shared Python logic in `tools/rules_kit/` instead of duplicating it inside subsystem
generators. Like the generators, `tools/rules_kit/` and `tools/test_kit/` are protected and
require explicit user approval before code changes.

---

## Quick Start

```bash
cmake -S . -B build
cmake --build build -j
./build/zithc --help
```

Requires:

- CMake 3.20+
- Python 3
- A C++23 compiler

The build runs the generators automatically and places generated files in the build tree.

---

## Why ZCT

This repository is built around one practical idea:

**when a subsystem is mostly structure, make the structure declarative.**

That gives a few concrete benefits:

- one place to change behavior instead of many handwritten files
- fewer parser and wiring mistakes
- generated code that stays consistent in naming and layout
- easier onboarding, because the intent lives in compact rule files
- better maintainability when features evolve

---

## Current Surface

The current executable is `zithc`, with a generated CLI surface that already exposes:

- global flags such as `--help`, `--version`, `--verbose`, `--strict`, `--opt-level`, `--include`, `--jobs`
- commands such as `check`, `fmt`, `run`, `build`, and `deps`
- subcommands such as `deps add`, `deps check`, and `deps remove`
- typo suggestions for commands, subcommands, and flags

`zithc check`, `zithc run`, and `zithc build` now create a `toolkit::session::CompilationSession`,
load the selected source file, and run the pipeline through the `Lexed` stage.

Example:

```bash
./build/zithc --help
./build/zithc --version
```

## Codegen Roadmap

The backend selector reserves five names: `tiny`, `tinyJit`, `vm`, `llvm`, and `llvmJit`.
Only `vm` is implemented in this phase and it is the default backend used by the codegen
demos. `tiny`, `tinyJit`, `llvm`, and `llvmJit` are roadmap entries only; enabling them through
the build is rejected until a milestone implements them.

Future `cache`, `ast`, `import`, and `symbols` improvements are intentionally incremental. They
must remain compatible with the current generated/rules boundary and are not planned as a
replacement architecture in this phase.

---

## Helper Guides

All helper guides below are intentionally written in English, so they can act as direct usage
documentation for contributors.

### 1. CLI Helper

**Purpose**

Generate a typed command-line interface from a compact rules file. This helper removes most of the
manual work normally required for option parsing, dispatch wiring, help generation, and typo
suggestions.

**Source files**

- `src/cli/cli.rules`
- `src/cli/generate.py`
- `src/cli/handlers.cpp`

Full usage details are in `src/cli/README.md`.

**Generated outputs**

- `build/src/cli/cli.hpp`
- `build/src/cli/cli.cpp`
- `build/src/cli/actions.hpp`
- `build/src/cli/.gitignore`

**How to use**

1. Edit `src/cli/cli.rules`.
2. Define global flags, commands, subcommands, arguments, and dispatch actions.
3. Rebuild the project, or run the generator manually.
4. Implement the referenced handlers in `src/cli/handlers.cpp`.

**Manual command**

```bash
python3 src/cli/generate.py src/cli/cli.rules --out build/src/cli
```

**Minimal example**

```text
[default]
-> show_help()

[flags]
h | help        -> show_help()
version         -> show_version()
v | verbose     = false
opt-level       = 0..3
I | include     = [] / ,

[commands]
build -> cmd::build(opts)
deps

[args]
input  : string
output : string = "out.txt"

[command-args]
build.input  : path
build.output : path = "a.out"
deps.add.package : string

[sub-commands]
deps: add -> cmd::depsAdd()
deps: remove -> cmd::depsRemove()

[sub-flags]
deps: add: force = false
```

**`cli.rules` syntax**

Sections currently recognized by the generator:

- `[default]`
- `[flags]`
- `[commands]`
- `[args]`
- `[command-args]`
- `[sub-commands]`
- `[sub-flags]`

Core forms:

```text
# default action
-> show_help()

# flag aliases + bool/int/range/string/list/enum/action
h | help         -> show_help()
verbose          = false
jobs             = 1
opt-level        = 0..3
output           = "a.out"
include          = [] / ,
color            = auto | off | on

# commands
build            -> cmd::build(opts)
deps

# global positional args
input            : string
output           : path = "out.txt"

# command or subcommand positional args
build.input      : path
run.args         : string...
deps.add.package : string

# subcommands
deps: add        -> cmd::depsAdd()
deps: check      -> cmd::depsCheck()

# command-level or subcommand-level flags
fmt: check       = false
deps: add: force = false
```

Notes:

- `opts` is currently sugar for `const Options &opts`.
- Longer-term, the exception for `opts` will disappear and actions will use ordinary signatures.
- Variadic arguments use `...` and must be the last positional argument.
- Supported argument types are `string`, `path`, `int`, `bool`, and enum choices like `auto | off | on`.
- Lists use `[] / <separator>`, for example `[] / ,`.

**What the rules file can describe**

- default action
- global flags
- commands
- command arguments
- subcommands
- per-command flags
- subcommand flags
- action dispatch signatures

**When to use it**

Use this helper whenever a new command, option, subcommand, or argument shape is needed. Prefer
changing the rules file instead of hand-editing parser logic.

---

### 2. Lexer Helper

**Purpose**

Generate lexer support code from a declarative token specification. This helper centralizes token
definitions, keyword mapping, punctuation and operator sets, numeric literal behavior, and optional
hooks or generated members.

**Source files**

- `src/frontend/lexer/lexer.rules`
- `src/frontend/lexer/generate.py`
- `src/frontend/lexer/types.hpp`
- `src/frontend/lexer/actions.cpp`

Full usage details are in `src/frontend/lexer/README.md`.

**Generated outputs**

- `build/src/frontend/lexer/lexer.hpp`
- `build/src/frontend/lexer/lexer.cpp`
- `build/src/frontend/lexer/actions.hpp`
- `build/src/frontend/lexer/keyword-table.hpp`
- `build/src/frontend/lexer/.gitignore`

**How to use**

1. Edit `src/frontend/lexer/lexer.rules`.
2. Define tokens, keywords, punctuation, operators, comments, and optional lexer/token members or hooks.
3. Rebuild the project, or run the generator manually.
4. Implement any declared hooks in `src/frontend/lexer/actions.cpp`.
5. Consume the generated lexer through the `zct_frontend_lexer` library target.

**Manual command**

```bash
python3 src/frontend/lexer/generate.py src/frontend/lexer/lexer.rules --out build/src/frontend/lexer --types src/frontend/lexer/types.hpp
```

**Minimal example**

```text
[tokens]
identifier = true
skip = " \n\t\r"
string = [escape-sequence = true, back-slash = true]
decimal = [under-score-divisor = true]
hexadecimal = true
binary = true
octal = true
punc = "{}()[]:;."
operators = "+-*/%<>=!&|^"
compound = ["+=", "-=", "==", "!="]
comments = [single = "//", multi = ["/*", "*/"]]

[keywords]
If = "if"
Else = "else"
Fn = "fn"
When = "when"

[lexer]
loc: Span = {0,0}

[token-type]
kindTag: int = 0

[actions]
onLex = hooks::onLex()
offLex = hooks::offLex()
onToken = hooks::onToken()
```

**`lexer.rules` syntax**

Sections currently recognized by the generator:

- `[tokens]`
- `[keywords]`
- `[lexer]`
- `[token-type]`
- `[actions]`

Core forms:

```text
[tokens]
identifier = true
skip = " \n\t\r"
punc = "{}()[]"
operators = "+-*/"
compound = ["+=", "==", "->"]
string = [escape-sequence = true, back-slash = true]
decimal = [under-score-divisor = true]
comments = [single = "//", multi = ["/*", "*/"]]
hexadecimal = true
binary = [prefix = "0b"]
octal = [prefix = "0c"]

[keywords]
If = "if"
Else = ["else", "otherwise"]

[lexer]
depth: int = 0

[token-type]
kindTag: int = 0

[actions]
onLex = hooks::onLex()
offLex = hooks::offLex()
onToken = hooks::onToken()
```

Notes:

- `identifier = true` is mandatory.
- If `[token-type]` is used, `[actions] onToken` is required.
- `[lexer]` defines generated members on the lexer object.
- `[token-type]` defines extra members on the generated `Token` structure.
- `[actions]` emits declarations into `build/src/frontend/lexer/actions.hpp`; the supported handwritten implementation point is `src/frontend/lexer/actions.cpp`.
- Keywords map a token kind name such as `If`, `Else`, or `Fn` to one string or a list of strings.

**What the rules file can describe**

- token families
- keyword-to-token mappings
- punctuation sets
- operator sets
- comment styles
- string and numeric literal options
- generated members and hooks

**When to use it**

Use this helper when token behavior changes. Keep lexical structure in the rules file rather than
spreading it across multiple handwritten lexer tables and conditionals.

---

### 3. Flags Helper

**Purpose**

Generate typed project configuration defaults and a TOML loader from a single declarative config
file. This makes project settings explicit, consistent, and easier to evolve without duplicating
default values in multiple places.

**Source files**

- `src/config/flags/default.toml`
- `src/config/project/generate.py`

**Generated outputs**

- `build/src/config/project/project-config.hpp`
- `build/src/config/project/project-config.cpp`
- `build/src/config/project/actions.hpp`
- `build/src/config/project/.gitignore`

**How to use**

1. Edit `src/config/flags/default.toml`.
2. Add or change fields inside the supported sections: `project`, `build`, `paths`, and `ffi`.
3. Rebuild the project, or run the generator manually.
4. Load TOML text into the generated `ProjectConfig` type.

**Manual command**

```bash
python3 src/config/project/generate.py src/config/flags/default.toml --out build/src/config/project
```

**Minimal example**

```toml
[project]
name = "zith"
version = "0.1.0"
description = "A toy language and compiler tooling project"
authors = ["Zith Authors"]
license = "MIT"
homepage = ""

[build]
entry = "src/main.zith"
output = "out/zith"
mode = "debug"
opt_level = 0
debug_level = 1
edition = "2026"
verbose = false
strict = false

[paths]
src_dir = "src"
asset_dir = "assets"
bin_dir = "bin"
mod_dir = "modules"
docs_dir = "docs"
test_dir = "tests"

[ffi]
include_dirs = ["include", "src"]
c_source_dirs = []
library_dirs = []
libraries = []
defines = []
```

**`default.toml` schema**

The config generator does not use a `.rules` file. Its source of truth is a TOML file with the
currently supported sections:

- `[project]`
- `[build]`
- `[paths]`
- `[ffi]`

Supported value types:

- `string`
- `int`
- `bool`
- `array of strings`

Notes:

- Unknown sections are rejected.
- Repeated fields are rejected.
- Arrays must contain only strings.
- Generated C++ field names are converted to camelCase.

**What it generates**

- a strongly typed `ProjectConfig` structure
- default values for all declared fields
- section-aware TOML parsing support
- string interning for string-based fields

**When to use it**

Use this helper whenever project defaults or configuration schema evolve. Prefer changing the TOML
schema once instead of duplicating config definitions in handwritten C++.

### 4. Session Helper

**Purpose**

Generate the `CompilationSession` pipeline from a declarative stage list, stage output types, and a
single injected context object. The generated session owns the pipeline mechanics and optional
session-local state, while compiler-owned services live in the user-defined context type declared in
`types.hpp`.

**Source files**

- `src/session/session.rules`
- `src/session/generate.py`
- `src/session/dispatch.cpp`

Full usage details are in `src/session/README.md`.

**Generated outputs**

- `build/src/session/session.hpp`
- `build/src/session/session.cpp`
- `build/src/session/dispatch.hpp`
- `build/src/session/.gitignore`

**How to use**

1. Edit `src/session/session.rules`.
2. Define the injected context type, stages, their output types, and any optional session-local
   state fields.
3. Rebuild the project, or run the generator manually.
4. Implement `dispatch<Stage>()` specializations in `src/session/dispatch.cpp`.

**Manual command**

```bash
python3 src/session/generate.py src/session/session.rules --out build/src/session
```

**What it generates**

- `Stage` and `PipelinePlan` types
- a `CompilationSession` with the configured context type and any session-local state
- `StageResult` with `run()`, `runTo()`, and `resume()`
- `dispatch<Stage>()` declarations for every configured stage

Stages share earlier results through the session's typed storage, not through
`dispatch<Stage>()` parameters. A handler reads a completed stage with
`hasStageResult<Stage>()` and `stageResult<Stage>()`; `run()`, `runTo()`, and `resume()`
are the only paths that populate those slots.

**When to use it**

Use this helper when the compiler pipeline stages, injected compiler context, session-local state,
or stage outputs change. Keep new pipeline plumbing declarative instead of hand-editing
`CompilationSession`.

### 5. Diagnostic Helper

**Purpose**

Generate `ErrorInfo`, `lookupError`, and `ErrorTemplate` from a small declarative error catalogue.
The common diagnostic runtime keeps the diagnostic type, renderer, and levenshtein helpers in
`src/common/diagnostic/`.

**Source files**

- `src/diagnostic/error.rules`
- `src/diagnostic/generate.py`

Full usage details are in `src/diagnostic/README.md`.

**Generated outputs**

- `build/src/diagnostic/error-info.hpp`
- `build/src/diagnostic/error-info.cpp`
- `build/src/diagnostic/.gitignore`

**How to use**

1. Edit `src/diagnostic/error.rules`.
2. Add or change error codes, severities, categories, titles, templates, and notes.
3. Rebuild the project, or run the generator manually.
4. Use `lookupError` and `ErrorTemplate` from the generated header through the common renderer.

**Manual command**

```bash
python3 src/diagnostic/generate.py src/diagnostic/error.rules --out build/src/diagnostic
```

**What it generates**

- constexpr `ErrorInfo` entries with stable numeric codes
- `lookupError` and `errorInfo` lookup helpers
- `ErrorTemplate::render` with `{message}` and `{lexeme}` substitution

**Template placeholders: `message` vs `lexeme`**

`renderDiagnostic` provides both placeholders for catalogue templates:

- `{message}` is the `Diagnostic::message` text supplied by the call site. It is the right
  placeholder when the rule only wraps a message that is already fully formed, as in `E4001`.
- `{lexeme}` is the raw source/token text extracted from the diagnostic `span` by
  `renderDiagnostic`. It is useful when the rule wants to keep fixed prose in the catalogue and
  only insert the offending token spelling, as in `E4002`.

When the diagnostic span is missing or outside the loaded source, `renderDiagnostic` substitutes
the literal `<invalid span>` for `{lexeme}`.

Examples:

| Placeholder | Rule template | `renderDiagnostic` output |
|---|---|---|
| `{message}` | `template = "{message}"` | `path:line:col: error: E4001: broken` |
| `{lexeme}` | `template = "unknown {lexeme}"` | `path:line:col: error: E4002: unknown +` |

Coded diagnostics use a compact first line followed by a rich block:

```text
path:line:col: error: E4001: <rendered template>
  --> path:line:col
  1 | let y = 0 ;
    |       ^ broken
  = note: <catalogue note or Diagnostic note>
```

The `-->` header, source context and `= note:` lines are emitted whenever a diagnostic has a
nonzero code. Diagnostics without a code keep the existing compact format with `note:` lines.

---

## Development Model

The intended workflow is:

1. describe structure in a compact source file
2. generate the repetitive C++ layer
3. keep handwritten code focused on behavior, not plumbing

In other words, Zith treats generators as maintainability tools, not as one-off scripts.

---

## Repository Layout

```text
.
├── readme.md
├── src/
│   ├── CMakeLists.txt
│   ├── app/             # executable entry point
│   ├── cli/             # declarative CLI + generator + handlers
│   ├── common/          # support runtime used by generated code
│   ├── diagnostic/      # declarative error catalogue + generator
│   ├── config/
│   │   └── project/     # declarative config schema + generator
│   ├── frontend/
│   │   ├── lexer/       # declarative lexer + generator + types
│   │   └── ast/         # declarative AST nodes + generator + types
│   │   └── parser/      # declarative parser + generator + hooks
│   ├── session/         # declarative compilation session + actions
│   └── support/
├── tests/
│   ├── common/
│   ├── frontend/
│   └── integration/
└── CMakeLists.txt
```

---

## Testing

The current test suite validates the generated CLI behavior, including:

- help output
- valid dispatch paths
- version output
- suggestions for mistyped commands
- suggestions for mistyped subcommands
- suggestions for mistyped flags

Run tests with:

```bash
ctest --test-dir build --output-on-failure
```

---

## Positioning

ZCT is the compiler toolkit; Zith is its showcase language. The public `zithc` entry point is
Zith-branded, while the internal libraries and build options use the `zct_*`/`ZCT_*` prefix.
The public roadmap is the five backend names above; everything else is maintained declaratively
so the handwritten compiler code can stay small, clear, and easier to evolve.
