# Native `zithc test` and the compiler-synthesized test driver

Status: accepted for the Zith-- language. It adds `#[test]`, `@assert`,
`@check`, and `@log`, and reworks the `zithc test` command.

`zithc test` was shipped as a batch type-check of the `.zith` files under
`test_dir`, with no execution and no way for a test to fail at runtime. That
made the command a duplicate of `zithc check` and left the Zith-- compiler with
no test execution surface of its own. This records the design that replaces it.

## The test unit is a `#[test]` function

A test is a free `void` function with no parameters, annotated `#[test]`. The
runner discovers these functions in each file under `test_dir` and reports one
result per function. A test file does not need a `main`, and `#[test]` on a
function with parameters or a non-void return is `W2029`. A module that
declares both `main` and `#[test]` is an error, because the synthesized driver
owns the entry point in test mode.

This reuses the existing attribute machinery. `test` becomes the third known
`AttributeKind`, alongside `discardable` and `volatile`.

## Failure travels through a compiler-synthesized result table

Zith-- has no mutable globals and no `Result`, and the native backend runs a
test as a separate process whose only guaranteed return channel is the exit
code. The design sidesteps all three constraints with one mechanism: the
compiler synthesizes a table with one slot per `#[test]` function, writes the
assert id into a slot on failure, and the driver prints the table at the end.

`@assert(cond)` is fail-fast. Inside a test, a false condition records its
failure ID in the result slot and returns from that test, so the driver
continues and fills every slot. Outside a test it lowers to a panic.
`@check(cond)` is non-fatal inside a test: each false condition emits a
failure event and execution continues. Outside the lexical body of a test it
has the same panic behavior as `@assert`. Check helper macros expand at their
call site so they inherit the test context; ordinary helper functions do not.
The result slot is `0` when no assert failed and `N` when the test returned at
the `N`th failure site. Check failures are reported separately from that slot.

The table is runner infrastructure, not a language feature. The user cannot
name or write it.

## The driver is synthesized as HIR, so every backend runs it

The compiler builds the test driver as HIR after semantic analysis, as a `main`
that calls each `#[test]` in declaration order. Codegen, VM v2, and the HIR
interpreter all run the same lowered driver, so the command works on every
backend without a per-backend entry point. This is why the runner does not need
a new "call function by name" API in the backends, which today expose only
`runMain`.

## Result protocol

The driver prints program output and test logs during the run, then a final
delimited block of result lines of the form `zith-test <name> <slot>`. The
runner reads only that last block and relays the rest. Log text is printed by
the driver and never parsed by the runner, so arbitrary test output cannot be
mistaken for a result line. The command exit code stays `0` or `1`.

The assert/check failure-ID-to-location mapping lives in the `Debug` section
of the ZIRL artifact. The runner already holds the artifact, so resolving a
reported failure to a source location is a local lookup.

## `@log` gives per-test output without per-process isolation

An earlier option was one process per test, which would have captured output
per test for free. The design keeps one process per file instead, and pays for
it with `@log(msg)`, an intrinsic that appends `[]char` to a per-test buffer.
The driver prints the log of failing tests by default and of all tests under
`--verbose`. This is what makes `std/test` able to show expected versus actual
values on failure. The buffer has fixed capacity in this iteration, with no
synthesized dynamic allocation.

## Consequences

- The `zithc test` command changes behavior. The `docs/impl-status.md` note
  "Discovers and runs test files under the given path" no longer describes it
  and must be rewritten.
- `@assert` reports only its first failure per test because it returns at that
  point. Earlier `@check` failures are preserved, while later checks do not
  run after a failing assert.
- A hard trap (segfault, division by zero) kills the file's process and the
  runner reports a crash for that file, losing the remaining tests in it.
- `test_dir` becomes documented project configuration, defaulting to `tests`.
