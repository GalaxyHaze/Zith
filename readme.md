# Zith `autonom`

> **Status: Experimental tooling branch.**
> `autonom` is a tooling-focused branch of Zith. Instead of presenting the language first,
> this branch focuses on making code production simpler, reducing handwritten boilerplate,
> and improving long-term maintainability through small generators and declarative inputs.

A large part of compiler and tooling code is repetitive: command-line parsing, token tables,
configuration defaults, typed config loading, and error-prone glue code. The goal of
`autonom` is to move those repetitive parts into compact source descriptions and let
small helpers generate the C++ implementation.

The result is a branch that aims to be:

- simpler to extend
- easier to review
- cheaper to refactor
- more consistent across modules
- less dependent on copy/paste patterns

---

## What This Branch Introduces

Inspired by the structure and readability of the `main` branch README, `autonom` reframes the
project as a set of code-generation helpers for building maintainable compiler infrastructure.

Today, the branch already includes three concrete helpers:

| Helper | Purpose | Source of truth |
|---|---|---|
| CLI helper | Generate command parsing, dispatch, help text, and suggestions | `src/cli/cli.rules` |
| Lexer helper | Generate lexer tables and tokenization support code | `src/lexer/lexer.rules` |
| Project-config helper | Generate strongly typed defaults and TOML loading code | `src/project-config/default.toml` |

Supporting those helpers is a small common runtime with arena allocation, interned strings,
results, option-like types, and dynamic arrays under `src/common`.

---

## Quick Start

```bash
git clone https://github.com/GalaxyHaze/Zith.git
cd Zith
git checkout autonom
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

## Why `autonom`

This branch is built around one practical idea:

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

Example:

```bash
./build/zithc --help
./build/zithc --version
```

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

**Generated outputs**

- `cli.hpp`
- `cli.cpp`
- `actions.hpp`
- `.gitignore`

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

- `src/lexer/lexer.rules`
- `src/lexer/generate.py`
- `src/lexer/types.hpp`

**Generated outputs**

- `lexer.hpp`
- `lexer.cpp`
- `actions.hpp`
- `keyword-table.hpp`
- `.gitignore`

**How to use**

1. Edit `src/lexer/lexer.rules`.
2. Define tokens, keywords, punctuation, operators, comments, and optional lexer/token members or hooks.
3. Rebuild the project, or run the generator manually.
4. Consume the generated lexer through the `lexer` library target.

**Manual command**

```bash
python3 src/lexer/generate.py src/lexer/lexer.rules --out build/src/lexer --types src/lexer/types.hpp
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

### 3. Project-Config Helper

**Purpose**

Generate typed project configuration defaults and a TOML loader from a single declarative config
file. This makes project settings explicit, consistent, and easier to evolve without duplicating
default values in multiple places.

**Source files**

- `src/project-config/default.toml`
- `src/project-config/generate.py`
- `src/project-config/project-config.cpp`
- `src/project-config/project-config.hpp`

**Generated outputs**

- `project-config.hpp`
- `project-config.cpp`
- `actions.hpp`
- `.gitignore`

**How to use**

1. Edit `src/project-config/default.toml`.
2. Add or change fields inside the supported sections: `project`, `build`, `paths`, and `ffi`.
3. Rebuild the project, or run the generator manually.
4. Load TOML text into the generated `ProjectConfig` type.

**Manual command**

```bash
python3 src/project-config/generate.py src/project-config/default.toml --out build/src/project-config
```

**Minimal example**

```toml
[project]
name = "zith"
version = "0.1.0"
description = "A systems language project"
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

---

## Development Model

The intended workflow in `autonom` is:

1. describe structure in a compact source file
2. generate the repetitive C++ layer
3. keep handwritten code focused on behavior, not plumbing

In other words, this branch treats generators as maintainability tools, not as one-off scripts.

---

## Repository Layout

```text
.
├── main.cpp
├── src/
│   ├── cli/             # declarative CLI + generator + handlers
│   ├── lexer/           # declarative lexer + generator + types
│   ├── project-config/  # declarative config schema + generator
│   ├── common/          # support runtime used by generated code
│   └── support/
├── tests/
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

`main` presents Zith primarily as a language project.

`autonom` presents Zith as a maintainability experiment: a toolkit for encoding repetitive
compiler/tooling structure declaratively, then generating the boring parts so the handwritten code
stays smaller, clearer, and easier to evolve.
