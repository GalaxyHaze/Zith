# CLI Helper

## Purpose

The CLI helper generates the command-line parsing layer for `zithc` from a compact declarative
rules file. It removes the repetitive C++ needed for flags, commands, subcommands, arguments,
help text, dispatch, and typo suggestions.

The source of truth is `cli.rules`. The generator should be changed whenever the CLI surface must
produce a different shape; hand-written C++ should only implement behavior.

## Files

| File | Responsibility |
|---|---|
| `cli.rules` | Declarative description of the CLI surface. |
| `generate.py` | Parser and C++ generator for the CLI surface. |
| `handlers.cpp` | Hand-written command, flag, and default-action implementations. |
| `terminal.cpp` / `terminal.hpp` | Terminal output helpers used by help/version handlers. |
| `build/src/cli/cli.hpp` | Generated public CLI API. |
| `build/src/cli/cli.cpp` | Generated parser and dispatch implementation. |
| `build/src/cli/actions.hpp` | Generated declarations for every action referenced in the rules. |

## Rules Syntax

### Sections

| Section | Purpose |
|---|---|
| `[default]` | Action invoked when no command is passed. |
| `[flags]` | Global flags available to every command. |
| `[commands]` | Top-level commands and their dispatch actions. |
| `[args]` | Global positional arguments. |
| `[command-args]` | Positional arguments scoped to a command. |
| `[sub-commands]` | Commands nested under a parent command. |
| `[sub-flags]` | Flags scoped to a command or to a subcommand. |

Unknown sections are rejected. Actions are declared as `->` targets and are also generated into
`actions.hpp`, so the handwritten implementation must match the generated signature.

### Global And Command Actions

```text
[default]
-> show_help()

[flags]
h | help        -> show_help()
version         -> show_version()
v | verbose     = false
opt-level       = 0..3
I | include     = [] / ,
j | jobs        = 1

[commands]
build           -> cmd::build(opts)
deps
```

An action either takes no arguments or takes exactly `opts`. `opts` is generated as
`const generated_cli::Options &`.

### Positional Arguments

```text
[args]
input       : string
output      : string = "out.txt"

[command-args]
build.input      : path
run.script       : string
run.args         : string...
deps.add.package : string
```

Supported argument kinds:

| Kind | Notes |
|---|---|
| `string` | Stored as an interned id. |
| `path` | Stored as an interned id; use when the value is a filesystem path. |
| `int` | Numeric value. |
| `bool` | `true` or `false`. |
| `A | B | C` | Enum with the given choices. |

`...` makes an argument variadic. A variadic argument must be the last argument in its command or
subcommand context and cannot have a default.

### Subcommands

```text
[sub-commands]
deps: add    -> cmd::depsAdd()
      check  -> cmd::depsCheck()
      remove -> cmd::depsRemove()

[sub-flags]
fmt: check         = false
deps: add: force   = false
```

After one explicit `parent: child`, later lines without a parent use the previous parent.

### Flag Kinds

| Form | Generated behavior |
|---|---|
| `name -> action()` | Action flag, not stored in options. |
| `name = true` / `false` | Boolean flag. |
| `name = 1` | Integer flag. |
| `name = 0..3` | Integer flag validated against a range. |
| `name = "value"` | String flag. |
| `name = auto \| off \| on` | Enum flag; first choice is the default. |
| `name = [] / <separator>` | List flag; default separator is `,`. |

## Common Workflow

1. Read `cli.rules` to understand the current CLI before editing.
2. Edit `cli.rules` first.
3. Regenerate C++:

```bash
python3 src/cli/generate.py src/cli/cli.rules --out build/src/cli
```

4. Declare or update matching functions in `src/cli/handlers.cpp`.
5. Rebuild and run the tests:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Normal project builds regenerate the CLI automatically because the generator and rules file are
CMake dependencies.

## Handwritten Handlers

`handlers.cpp` is the only intended handwritten implementation point for CLI behavior. Keep it
focused on behavior; do not add parsing or dispatch logic there.

Example action implemented as a subcommand:

```cpp
namespace cmd {

int depsAdd() {
    // Implement dependency insertion behavior here.
    return 0;
}

} // namespace cmd
```

If the action receives `opts`, the generated declaration and the handwritten definition must both
use `const generated_cli::Options &`.

## Agent Boundary

Normal CLI changes belong in `src/cli/cli.rules` and `src/cli/handlers.cpp`. Do not edit
`build/src/cli/*`; those files are rebuilt from `cli.rules`. Do not modify `generate.py` or
shared generator rules without explicit user approval.

## Tests

The CLI behavior tests live in `tests/integration` and cover help/version output, valid dispatch,
typo suggestions, and command flags. Run the relevant subset:

```bash
ctest --test-dir build -R cli --output-on-failure
```

## Demo

`tests/cli/cli-demo.cpp` builds a `generated_cli::Cli` and exercises `--help`, `--version`, and a
`deps add` command through `parseArgs` and `dispatch`.

```bash
cmake --build build --target cli-demo -j
ctest --test-dir build -R '^cli-demo$' --output-on-failure
```
