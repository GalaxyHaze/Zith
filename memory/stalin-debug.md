# Stalin Debug

Summary: strategy for narrowing compiler bugs by disabling parts of the
reproducer/code before reaching for a debugger or reading the whole tree.
Use this for crashes and regressions, especially when the failing pipeline
stage is already known.

## Process

1. Record the exact failing command, input, and observed error.
2. Remove parts of the failing example until the error disappears, then
   restore the smallest failing difference.
3. If the compiler has pipeline stages, run the same input through the
   narrowest stages that reproduce the crash (`check`, `--emit-ir`, `run`).
4. Test one suspect at a time, not several simultaneously.
5. Only when the minimal reproducer is stable, inspect the code paths touched
   by that input area.
6. Re-enable/restore pre-existing user files that were only removed during
   the experiment; do not ship debug-only edits.

## Example

`zithc run main.zith` crashed with a trace/breakpoint trap only when the file
imported `from soon/string`. `check` passed, `--emit-ir` also crashed, so the
problem was narrowed to HIR/codegen rather than sema or linking. The next split
tests a synthetic module with a simple struct before the real `string.zith`,
then adds fields/optional pointers one at a time.
