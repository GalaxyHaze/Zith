# Contributing to Zith Compiler's Toolkit

ZCT is a derived distribution of the original Zith project. Before opening a
contribution, read `README.md`; they document the declarative helpers,
the expected workflow, and the source of truth for each generated subsystem.

## Attribution

Contributors and forks must preserve the following attributions:

- The compiler code is based on the original source at
  [GalaxyHaze/Zith](https://github.com/GalaxyHaze/Zith), licensed under the MIT License.
- The generator-based tooling described in this branch, including the "Zith Compiler's
  Tools", is derived from [GalaxyHaze/Zith-Lang](https://github.com/GalaxyHaze/Zith-Lang).
  Keep the "Zith Compiler's Tools" name and cite this source when distributing or changing
  the generators and their documentation.

Do not remove or weaken these credits in derived code, generated artifacts,
documentation, or fork metadata. The full license terms are in `LICENSE`.

## Generated Code

This branch keeps repetitive compiler and tooling structure in declarative rules files and
regenerates the C++ from small generators. Do not hand-edit generated parser, lexer, config,
CLI, AST, or session code.

When a change affects a generated subsystem:

1. Find the corresponding rules file and generator under `src/<subsystem>/`.
2. Change the declarative source or the generator first.
3. Regenerate the output by rebuilding, or by running the documented manual generator command.
4. Build and test the generated result, not only the generator source.

Keep handwritten code focused on behavior rather than plumbing. Table wiring, option parsing,
and repetitive generated glue usually belong in the rules file and generator, not in
handwritten source.

## Development Workflow

1. Check out the repository.
2. Read `README:md` for the subsystem you are changing.
3. Change the declarative rules or generator source first.
4. Regenerate and build:

```bash
cmake --build build -j
```

5. Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

6. Keep changes small enough that behavior and rollout can be reviewed incrementally.

## Building

Requirements:

- CMake 3.20+
- Python 3
- A C++23 compiler

Configure and build from a fresh checkout:

```bash
cmake -S . -B build
cmake --build build -j
./build/turvc --help
```

Generators run automatically during the build and place generated files in the build tree.

## Project Structure

- `readme.md` documents the current helpers and the declarative source of truth for each area.
- `src/cli` contains the CLI rules, generator, handlers, and helper guide.
- `src/frontend/lexer` contains the lexer rules, generator, and helper guide.
- `src/frontend/ast` contains the AST rules, generator, and helper guide.
- `src/config/project` contains the project-config defaults, generator, and helper guide.
- `src/session` contains the compilation-pipeline rules, generator, and helper guide.
- `src/common` holds the small runtime types shared by handwritten code.
- `build` contains generated outputs and the built executable; it is not a source of truth.

## Code Style

- Prefer the local common runtime (`DynArray`, `Arena`, `FlatMap`, `Optional`, `Result`,
  `StringInterner`) over `std::` containers for internal state.
- Use `std::string_view`, `std::array` in generated tables, and standard library types only
  when the local facility does not apply.
- Keep the app entry point intentionally thin.
- Prefer one clear mechanism over layering several abstractions for the same problem.
- Preserve generated output by regenerating it; never fix generated files by hand.
