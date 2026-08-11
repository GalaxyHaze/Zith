"""Generator invocation and C++ smoke-test helpers."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def compile_smoke(
    repo_root: Path,
    compiler: str,
    include_root: Path,
    source: Path | None = None,
    include_dirs: list[Path] | None = None,
) -> None:
    if source is None:
        source = include_root / "smoke.cpp"
    command = [
        compiler,
        "-std=c++23",
        "-fsyntax-only",
        "-I",
        str(include_root),
        "-I",
        str(repo_root / "src"),
    ]
    if include_dirs:
        for include_dir in include_dirs:
            command += ["-I", str(include_dir)]
    command.append(str(source))
    subprocess.run(command, check=True, cwd=repo_root)


def write_rules(base: Path, text: str, generator: str | None = None) -> Path:
    if generator is None:
        path = base / "rules.rules"
    else:
        path = base / f"{generator}.rules"
    path.write_text(text, encoding="utf-8")
    return path


def generator_path(repo_root: Path, rules_path: Path) -> Path:
    relative = rules_path.name
    if relative in {"ast.rules", "rules.rules"}:
        return repo_root / "src/frontend/ast/generate.py"
    if relative == "lexer.rules":
        return repo_root / "src/frontend/lexer/generate.py"
    if relative == "parser.rules":
        return repo_root / "src/frontend/parser/generate.py"
    if relative == "session.rules":
        return repo_root / "src/session/generate.py"
    raise ValueError(f"cannot infer generator for rules file: {relative!r}")


def run_generator(
    repo_root: Path,
    rules_path: Path,
    out_dir: Path,
    types_path: Path | None = None,
    generator: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable,
        str(generator if generator is not None else generator_path(repo_root, rules_path)),
        str(rules_path),
        "--out",
        str(out_dir),
    ]
    if types_path is not None:
        command += ["--types", str(types_path)]
    return subprocess.run(
        command,
        text=True,
        capture_output=True,
        cwd=repo_root,
    )


def expect_error(
    repo_root: Path,
    tmpdir: Path,
    rules_text: str,
    needle: str,
    label: str,
    *,
    out_dir: Path | None = None,
    generator: Path | None = None,
    generator_kind: str | None = None,
    supports_types: bool = False,
    types_path: Path | None = None,
) -> None:
    if generator_kind is None:
        generator_kind = "parser" if generator is not None else None
    rules_path = write_rules(tmpdir, rules_text, generator_kind)
    output = out_dir or (tmpdir / "out")
    if generator is None:
        generator = generator_path(repo_root, rules_path)
    output.mkdir(parents=True, exist_ok=True)
    if supports_types and types_path is not None:
        result = run_generator(
            repo_root,
            rules_path,
            output,
            types_path=types_path,
            generator=generator,
        )
    else:
        result = run_generator(
            repo_root,
            rules_path,
            output,
            generator=generator,
        )
    if result.returncode == 0:
        raise AssertionError(f"generator accepted {label}")
    if needle not in result.stderr:
        raise AssertionError(f"{label}: missing {needle!r} in stderr: {result.stderr}")
