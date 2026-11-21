# Zith Test System: Comparison and Design Options

**Access date:** 2026-10-28

This is a design comparison, not a proposal to copy another runner. “Zith now”
describes the repository baseline in [`impl-status.md`](../impl-status.md#L198)
and [ADR 0045](../adr/0045-native-test-command-and-driver.md#L68).

## Comparison

- **Discovery and granularity.** **Zith now:** discovers `.zithm` files below
  `test_dir`; each `#[test]` is a free, zero-argument, `void` function. The
  compiler builds one driver per source module. [Rust] uses free `#[test]`
  functions and a compiler-generated `libtest` harness; [Go] discovers
  `TestXxx(*testing.T)` functions in `_test.go` files. [pytest] collects
  `test_*.py` / `*_test.py` files and `test`-prefixed functions and methods.
  [Vitest] discovers `.test.*` / `.spec.*` files and allows named, grouped
  tests inside them.
  ([Zith](../impl-status.md#L198), [Rust](https://doc.rust-lang.org/rustc/tests/index.html),
  [Go](https://pkg.go.dev/testing), [pytest](https://docs.pytest.org/en/stable/explanation/goodpractices.html),
  [Vitest](https://vitest.dev/guide/learn/writing-tests))

- **Execution and failure isolation.** **Zith now:** compiles and runs a child
  process per file, not per test. The first failed assertion ends that test;
  a hard trap can kill the file process and prevent later tests in that file
  from running. [Rust] runs tests in one test binary, catches test panics
  under the unwind strategy, and runs tests in parallel by default. [Go]
  distinguishes non-fatal `Error`/`Errorf` from `Fatal`/`Fatalf`; `T.Run`
  creates independently named subtests. [Vitest] isolates files in workers
  (child processes by default); tests within a file are sequential by default.
  ([Zith](../adr/0045-native-test-command-and-driver.md#L68),
  [Rust](https://doc.rust-lang.org/rustc/tests/index.html),
  [Go](https://pkg.go.dev/testing),
  [Vitest](https://vitest.dev/guide/parallelism))

- **Assertions and diagnostics.** **Zith now:** `@assert`, `@log`, and
  `std/test` macros provide source locations and formatted helper failures,
  but only the first failed assertion is reported per test.
  [Rust] supplies `assert!`/`assert_eq!` and treats panic as failure;
  [Go] can record a failure and continue with `Errorf`, or stop with `Fatalf`.
  [pytest] rewrites ordinary `assert` statements to show useful subexpression
  values. [Vitest] reports expected/received values, diffs, and the failing
  source line.
  ([Zith](../../stdlib/std/test.zithm),
  [Rust](https://doc.rust-lang.org/rustc/tests/index.html),
  [Go](https://pkg.go.dev/testing),
  [pytest](https://docs.pytest.org/en/stable/how-to/assert.html),
  [Vitest](https://vitest.dev/guide/learn/writing-tests))

- **Filtering and selection.** **Zith now:** case-sensitive substring match
  against `module::function`; no match is an error.
  [Rust] accepts test-name substrings. [Go] `-run` accepts an unanchored
  regular expression and matches hierarchical subtest name components.
  [pytest] provides file/node selection and `-k` boolean keyword expressions.
  [Vitest] filters by file, regex over the full test name, source line, or tag.
  ([Zith](../impl-status.md#L198),
  [Rust](https://doc.rust-lang.org/book/ch11-02-running-tests.html),
  [Go](https://pkg.go.dev/testing),
  [pytest](https://docs.pytest.org/en/stable/how-to/usage.html),
  [Vitest](https://vitest.dev/guide/filtering))

- **Fixtures and parameterization.** **Zith now:** has no runner-managed
  fixtures or parameterized test instances; helpers, macros, and loops remain
  ordinary language constructs. [Go] offers `T.Run` subtests and `T.Cleanup`.
  [pytest] has parameterized test instances and fixtures with function through
  session scopes. [Vitest] has `test.for` data cases and lifecycle hooks.
  ([Go](https://pkg.go.dev/testing),
  [pytest fixtures](https://docs.pytest.org/en/stable/how-to/fixtures.html),
  [pytest parametrization](https://docs.pytest.org/en/stable/how-to/parametrize.html),
  [Vitest](https://vitest.dev/guide/learn/setup-teardown))

- **Output and reporting.** **Zith now:** prints per-test outcomes and a
  summary; failing logs are shown by default, `--verbose` also shows passing
  logs, and displayed logs are capped at 4 KiB per test. The runner parses
  framed child output. [Rust] captures passing-test stdout and can reveal it
  with `--show-output`. [Go] prints logs on failure or with `-v`, and exposes
  `go test -json` for machine-readable events. [pytest] captures output by
  default and includes assertion detail in failure reports. [Vitest] reports
  tests and files with durations and supports configurable reporters.
  ([Zith](../impl-status.md#L198),
  [Rust](https://doc.rust-lang.org/book/ch11-02-running-tests.html),
  [Go](https://pkg.go.dev/testing),
  [pytest capture](https://docs.pytest.org/en/stable/how-to/capture-stdout-stderr.html),
  [Vitest](https://vitest.dev/guide/learn/writing-tests))

- **Parallelism.** **Zith now:** has a process boundary per file, but no
  documented test-parallelism control. [Rust] runs tests in parallel by
  default, configurable by thread count. [Go] makes parallel participation
  explicit with `T.Parallel`. pytest’s core execution is serial; the separate
  `pytest-xdist` plugin distributes tests across worker processes. [Vitest]
  runs files in parallel by default and supports opt-in concurrency within a
  file.
  ([Zith](../impl-status.md#L198),
  [Rust](https://doc.rust-lang.org/book/ch11-02-running-tests.html),
  [Go](https://pkg.go.dev/testing),
  [pytest-xdist](https://pytest-xdist.readthedocs.io/en/stable/distribution.html),
  [Vitest](https://vitest.dev/guide/parallelism))

- **Advanced testing.** **Zith now:** has no documented runner support for
  property testing, fuzzing, or snapshots. Go’s standard `testing` package
  includes fuzz targets and seed corpora. Snapshot testing is built into
  Vitest; JavaScript/TypeScript property testing is commonly integrated as a
  separate library such as fast-check. Python’s Hypothesis similarly
  integrates property-based tests with pytest rather than being pytest’s
  built-in generator.
  ([Go](https://pkg.go.dev/testing),
  [Vitest snapshots](https://vitest.dev/guide/learn/snapshots),
  [fast-check with Vitest](https://fast-check.dev/docs/tutorials/setting-up-your-test-environment/property-based-testing-with-vitest/),
  [Hypothesis](https://hypothesis.readthedocs.io/en/latest/quickstart.html))

## Design Options for Zith

These are staged choices for discussion, not a wholesale replacement design.

1. **Priority 0 — Make failure semantics and backend parity explicit.** Keep
   existing fail-fast assertions, or add a separate non-fatal `expect*`
   family that records multiple failures before the test ends. Whichever
   semantics are chosen, make diagnostics consistent across native, VM, and
   interpreter; the current interpreter has a known gap in rich
   `std/test` formatting ([baseline](../impl-status.md#L198)).

2. **Priority 1 — Improve crash isolation without recompiling every test.**
   Keep compilation per file, but consider selecting one test in the
   synthesized driver and invoking that binary per test. This preserves later
   tests after a hard trap at the cost of process startup. Alternatively,
   retain per-file execution and clearly report that a crash may skip the
   remaining tests in that file.

3. **Priority 1 — Add test identities before adding complex selection.**
   Consider `--list` and exact-name selection while preserving today’s
   substring `--filter` for compatibility. If parameterized cases are added,
   give each case a stable displayed/filterable identity so a failure is
   independently rerunnable. Tags or boolean filter expressions can wait
   until real suites need them.

4. **Priority 2 — Prefer small, explicit lifecycle features.** Start with
   test/module setup and guaranteed cleanup only when resource-heavy tests
   justify it. A general dependency-injected fixture graph like pytest is
   powerful, but brings scope, ordering, and caching semantics that the
   current zero-argument test model does not need yet.

5. **Priority 3 — Add advanced modes as separate capabilities.** If broad
   input coverage becomes important, evaluate property testing or fuzzing as a
   mode that saves reproducible failing inputs. For compiler output and
   diagnostics, consider reviewed golden/snapshot fixtures with explicit
   update behavior. Neither needs to complicate ordinary `#[test]` execution.

**Practical discussion order:** first decide whether tests should accumulate
failures or stop at the first assertion, then whether a hard crash must
preserve the rest of its file, and only then decide if parameterization and
fixtures are worth expanding the language surface.
