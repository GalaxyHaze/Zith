# Cross-Language Runtime Baseline

This directory compares small native runtime kernels implemented in Zith--, C,
and Rust. Zig is intentionally excluded from this first baseline.

The initial workloads are:

- `xorshift64`: a dependency-chain integer workload.
- `branch-count`: count values divisible by 7 or 11.

Each source is compiled once per run of the driver. C uses `cc -std=c17 -O3`,
Rust uses `rustc --edition=2024 -C opt-level=3`, and Zith-- uses
`zithc build --mode release --opt-level 3 --no-cache`. The runner checks each
program against an independently computed full-width checksum before timing it.
Compilation is excluded from measurements. Timings are wall-clock process
launches, so they include each executable's startup and scheduler noise.

Run a small correctness smoke test from the repository root:

```sh
agent-run -t 900 -c 0 -- python3 benchmarks/cross-language/run.py \
    --iterations 1000 --warmups 0 --repeats 1
```

Run and save a local baseline:

```sh
agent-run -t 900 -c 0 -- python3 benchmarks/cross-language/run.py \
    --iterations 50000000 --warmups 2 --repeats 7 \
    --json-out benchmarks/cross-language/baseline.json
```

The JSON file records the date, platform, architecture, CPU count, compiler
versions, configuration, raw samples, and medians. It is a machine-specific
snapshot, not a portable performance promise. Re-run the same command after
compiler changes to build a comparable history on the same machine.

The C compiler defaults to `cc` and can be overridden with `CC`. Rust defaults
to `rustc` and can be overridden with `RUSTC`. The Zith-- compiler defaults to
`./build/zithc` when present, otherwise `zithc` from `PATH`; override it with
`--zithc`.

This suite is intentionally standalone. The current
`ZITH_BUILD_BENCHMARKS=ON` CMake path fails because its archived lexer
benchmark sources have been removed.
