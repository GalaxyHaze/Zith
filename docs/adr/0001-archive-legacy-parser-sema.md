# Archive Legacy Parser and Sema Out of the Active Tree

The `Zith--` compiler should no longer compile, link, or mention `lib/legacy-zith/`; that code moves physically to a directory outside the project until it is either ported to the modern pipeline or abandoned. Keeping the legacy tree available as an archive instead of deleting it preserves the option to mine it for behavior or tests, while making the modern pipeline the single compiler surface.

Status: accepted

Considered Options:

The archive can live outside the repository, or the code can be deleted outright. Deleting is irreversible without git archaeology; leaving it inside the repo under `archive/` would keep it visible to tooling and still create accidental imports. A sibling directory outside the project avoids both pitfalls.

Consequences:

- CMake `ZITH_BUILD_LEGACY_LIB`, legacy sources, and legacy-only tests are removed from the active build.
- `src/`, modern `tests/`, and docs should stop naming `legacy-zith` as an implementation dependency.
- Since this affects the repository layout, the archive move itself is expected to be done without committing removed source files in the active tree.
