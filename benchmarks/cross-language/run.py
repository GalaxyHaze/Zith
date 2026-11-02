#!/usr/bin/env python3
"""Build, validate, and time the Zith--, C, and Rust runtime kernels."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent
LANGUAGES = ("c", "rust", "zith")
SOURCE_SUFFIXES = {"c": ".c", "rust": ".rs", "zith": ".zith"}
WORKLOADS = ("xorshift64", "branch-count")
MASK64 = (1 << 64) - 1


class BenchmarkError(Exception):
    """An actionable benchmark setup, build, or validation failure."""


def resolve_tool(spec: str, label: str) -> list[str]:
    parts = shlex.split(spec)
    if not parts:
        raise BenchmarkError(f"{label} command is empty")

    executable = parts[0]
    path = Path(executable)
    if path.is_absolute():
        resolved = path if path.is_file() else None
    elif "/" in executable:
        candidate = Path(os.path.abspath(Path.cwd() / path))
        if not candidate.exists():
            candidate = Path(os.path.abspath(ROOT.parents[1] / path))
        resolved = candidate if candidate.is_file() else None
    else:
        found = shutil.which(executable)
        resolved = Path(found) if found else None

    if resolved is None:
        raise BenchmarkError(f"{label} compiler not found: {executable}")
    # Keep symlink names such as `rustc`; rustup dispatches by argv[0].
    parts[0] = os.path.abspath(str(resolved))
    return parts


def compiler_commands(zithc_spec: str | None) -> dict[str, list[str]]:
    c_command = os.environ.get("CC", "cc")
    rust_command = os.environ.get("RUSTC", "rustc")
    if zithc_spec:
        zith_command = zithc_spec
    else:
        local_zithc = ROOT.parents[1] / "build" / "zithc"
        zith_command = str(local_zithc) if local_zithc.is_file() else "zithc"

    return {
        "c": resolve_tool(c_command, "C"),
        "rust": resolve_tool(rust_command, "Rust"),
        "zith": resolve_tool(zith_command, "Zith--"),
    }


def invoke(command: list[str], cwd: Path, timeout: int = 120) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            cwd=cwd,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise BenchmarkError(f"could not run {shlex.join(command)}: {exc}") from exc


def require_success(
    command: list[str], cwd: Path, language: str, workload: str
) -> subprocess.CompletedProcess[str]:
    result = invoke(command, cwd)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "(no compiler output)"
        raise BenchmarkError(
            f"{language} failed for {workload} (exit {result.returncode}):\n{detail}"
        )
    return result


def tool_version(command: list[str], cwd: Path) -> str:
    result = invoke([*command, "--version"], cwd)
    output = (result.stdout or result.stderr).strip()
    if result.returncode != 0 or not output:
        raise BenchmarkError(
            f"could not query compiler version for {shlex.join(command)}: "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )
    return output.splitlines()[0]


def expected_result(workload: str, iterations: int) -> int:
    if workload == "xorshift64":
        state = 88172645463325252
        for _ in range(iterations):
            state ^= (state << 13) & MASK64
            state &= MASK64
            state ^= state >> 7
            state &= MASK64
            state ^= (state << 17) & MASK64
            state &= MASK64
        return state

    if workload == "branch-count":
        return sum(1 for value in range(iterations) if value % 7 == 0 or value % 11 == 0)

    raise BenchmarkError(f"unknown workload: {workload}")


def compile_program(
    language: str,
    workload: str,
    iterations: int,
    expected: int,
    tool: list[str],
    workspace: Path,
) -> Path:
    source_path = ROOT / "workloads" / workload / f"{language}{SOURCE_SUFFIXES[language]}"
    try:
        source = source_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise BenchmarkError(f"cannot read {source_path}: {exc}") from exc

    if "@@ITERATIONS@@" not in source or "@@EXPECTED@@" not in source:
        raise BenchmarkError(f"{source_path} must contain both source markers")
    rendered = source.replace("@@ITERATIONS@@", str(iterations))
    rendered = rendered.replace("@@EXPECTED@@", str(expected))
    staged_source = workspace / f"{workload}-{language}{SOURCE_SUFFIXES[language]}"
    staged_source.write_text(rendered, encoding="utf-8")
    executable = workspace / f"{workload}-{language}"

    if language == "c":
        command = [*tool, "-std=c17", "-O3", "-Wall", "-Wextra", "-Werror",
                   str(staged_source), "-o", str(executable)]
    elif language == "rust":
        command = [*tool, "--edition=2024", "-C", "opt-level=3", "-D", "warnings",
                   str(staged_source), "-o", str(executable)]
    else:
        command = [*tool, "build", str(staged_source), "--mode", "release",
                   "--opt-level", "3", "--no-cache", "-o", str(executable)]

    require_success(command, workspace, language, workload)
    if not executable.is_file():
        raise BenchmarkError(f"{language} did not produce an executable for {workload}")
    return executable


def run_once(executable: Path, language: str, workload: str) -> int:
    start = time.perf_counter_ns()
    try:
        result = subprocess.run(
            [str(executable)],
            cwd=executable.parent,
            check=False,
            capture_output=True,
            timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise BenchmarkError(f"{language} could not run {workload}: {exc}") from exc
    elapsed_ns = time.perf_counter_ns() - start
    if result.returncode != 0:
        detail = result.stderr.decode(errors="replace").strip()
        raise BenchmarkError(
            f"{language} checksum validation failed for {workload} "
            f"(exit {result.returncode}); stderr: {detail or '(empty)'}"
        )
    if result.stdout or result.stderr:
        raise BenchmarkError(f"{language} {workload} unexpectedly wrote output")
    return elapsed_ns


def selected_workloads(name: str) -> tuple[str, ...]:
    return WORKLOADS if name == "all" else (name,)


def run_benchmarks(args: argparse.Namespace) -> dict:
    tools = compiler_commands(args.zithc)
    versions = {
        language: tool_version(tools[language], ROOT)
        for language in LANGUAGES
    }
    output: list[dict] = []

    for workload in selected_workloads(args.benchmark):
        expected = expected_result(workload, args.iterations)
        samples: dict[str, list[int]] = {language: [] for language in LANGUAGES}
        with tempfile.TemporaryDirectory(prefix=f"zith-bench-{workload}-") as temp:
            workspace = Path(temp)
            executables = {
                language: compile_program(
                    language, workload, args.iterations, expected, tools[language], workspace
                )
                for language in LANGUAGES
            }

            for language in LANGUAGES:
                for _ in range(args.warmups):
                    run_once(executables[language], language, workload)

            for sample_index in range(args.repeats):
                rotation = sample_index % len(LANGUAGES)
                order = LANGUAGES[rotation:] + LANGUAGES[:rotation]
                for language in order:
                    samples[language].append(
                        run_once(executables[language], language, workload)
                    )

        for language in LANGUAGES:
            values = samples[language]
            record = {
                "workload": workload,
                "language": language,
                "expected_checksum": expected,
                "samples_ns": values,
                "median_ns": int(statistics.median(values)),
                "min_ns": min(values),
            }
            output.append(record)
            print(
                f"{workload:14} {language:5} median={record['median_ns'] / 1e6:9.3f} ms "
                f"min={record['min_ns'] / 1e6:9.3f} ms checksum={expected}"
            )

    return {
        "schema_version": 1,
        "recorded_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor() or "unknown",
            "cpu_count": os.cpu_count(),
        },
        "configuration": {
            "iterations": args.iterations,
            "warmups": args.warmups,
            "repeats": args.repeats,
            "timing": "wall-clock process launch; includes executable startup",
        },
        "toolchains": {
            language: {
                "command": shlex.join(tools[language]),
                "version": versions[language],
            }
            for language in LANGUAGES
        },
        "results": output,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", choices=("all", *WORKLOADS), default="all")
    parser.add_argument("--iterations", type=int, default=10_000_000)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--zithc", help="Zith-- compiler command or executable path")
    args = parser.parse_args()
    if args.iterations <= 0:
        parser.error("--iterations must be positive")
    if args.warmups < 0:
        parser.error("--warmups cannot be negative")
    if args.repeats <= 0:
        parser.error("--repeats must be positive")
    return args


def main() -> int:
    args = parse_args()
    try:
        result = run_benchmarks(args)
        if args.json_out is not None:
            output_path = args.json_out.expanduser().resolve()
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            print(f"wrote {output_path}")
    except BenchmarkError as exc:
        print(f"benchmark error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
