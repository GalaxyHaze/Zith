#!/usr/bin/env python3

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


VALID_RULES = """\
[context]
type: sample::TestContext

[stages]
Parse: void
Emit: int
"""

MISSING_CONTEXT_RULES = """\
[stages]
Parse: void
"""

MULTIPLE_CONTEXT_RULES = """\
[context]
type: sample::FirstContext
type: sample::SecondContext

[stages]
Parse: void
"""

INVALID_CONTEXT_RULES = """\
[context]
type: 123bad

[stages]
Parse: void
"""

TYPES_HPP = """\
#pragma once

namespace sample {
struct TestContext {
    int value = 7;
};
} // namespace sample
"""

SMOKE_CPP = """\
#include "session/dispatch.hpp"

template <>
memory::Result<zith::session::ParseResult>
zith::session::dispatch<zith::session::Stage::Parse>(CompilationSession &) {
    return {};
}

template <>
memory::Result<zith::session::EmitResult>
zith::session::dispatch<zith::session::Stage::Emit>(CompilationSession &session) {
    return session.context().value;
}

int main() {
    sample::TestContext context{};
    zith::session::CompilationSession session(context);

    const auto parse = zith::session::dispatch<zith::session::Stage::Parse>(session);
    const auto emit = zith::session::dispatch<zith::session::Stage::Emit>(session);

    return (parse && emit && emit.value() == 7 && &session.context() == &context) ? 0 : 1;
}
"""


def assert_contains(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle!r}")


def assert_not_contains(text: str, needle: str, label: str) -> None:
    if needle in text:
        raise AssertionError(f"unexpected {label}: {needle!r}")


def write_rules(base: Path, text: str) -> Path:
    rules_path = base / "session.rules"
    rules_path.write_text(text, encoding="utf-8")
    return rules_path


def run_generator(repo_root: Path, rules_path: Path, out_dir: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(repo_root / "src/session/generate.py"),
            str(rules_path),
            "--out",
            str(out_dir),
        ],
        text=True,
        capture_output=True,
        cwd=repo_root,
    )


def compile_smoke(repo_root: Path, compiler: str, include_root: Path) -> None:
    source = include_root / "smoke.cpp"
    source.write_text(SMOKE_CPP, encoding="utf-8")
    subprocess.run(
        [
            compiler,
            "-std=c++23",
            "-fsyntax-only",
            "-I",
            str(include_root),
            "-I",
            str(repo_root / "src"),
            str(source),
        ],
        check=True,
        cwd=repo_root,
    )


def main() -> int:
    repo_root = Path(sys.argv[1]).resolve()
    compiler = sys.argv[2]

    with tempfile.TemporaryDirectory(prefix="session-generator-", dir="/tmp") as tmp:
        tmpdir = Path(tmp)
        include_root = tmpdir / "include"
        out_dir = include_root / "session"
        out_dir.mkdir(parents=True)
        (out_dir / "types.hpp").write_text(TYPES_HPP, encoding="utf-8")

        rules_path = write_rules(tmpdir, VALID_RULES)
        generated = run_generator(repo_root, rules_path, out_dir)
        if generated.returncode != 0:
            raise AssertionError(generated.stderr)

        session_hpp = (out_dir / "session.hpp").read_text(encoding="utf-8")
        session_cpp = (out_dir / "session.cpp").read_text(encoding="utf-8")
        dispatch_hpp = (out_dir / "dispatch.hpp").read_text(encoding="utf-8")

        assert_contains(session_hpp, "using Context = sample::TestContext;", "context alias")
        assert_contains(
            session_hpp,
            "explicit CompilationSession(Context &context);",
            "context constructor",
        )
        assert_contains(session_hpp, "Context &context() noexcept", "context getter")
        assert_contains(session_cpp, "plan.current = Stage::Parse;", "first-stage reset")
        assert_contains(
            dispatch_hpp,
            "using EmitResult = dispatch_result<Stage::Emit>::type;",
            "non-void dispatch alias",
        )
        assert_not_contains(session_hpp, "setFilePath(", "legacy file setter")
        assert_not_contains(session_hpp, "sourceMap()", "legacy source map accessor")

        compile_smoke(repo_root, compiler, include_root)

        missing = run_generator(repo_root, write_rules(tmpdir, MISSING_CONTEXT_RULES), out_dir)
        if missing.returncode == 0:
            raise AssertionError("generator accepted rules without [context]")
        assert_contains(
            missing.stderr,
            "[context] tem de declarar exatamente um tipo",
            "missing context error",
        )

        multiple = run_generator(repo_root, write_rules(tmpdir, MULTIPLE_CONTEXT_RULES), out_dir)
        if multiple.returncode == 0:
            raise AssertionError("generator accepted multiple context declarations")
        assert_contains(
            multiple.stderr,
            "[context] aceita exatamente um tipo",
            "multiple context error",
        )

        invalid = run_generator(repo_root, write_rules(tmpdir, INVALID_CONTEXT_RULES), out_dir)
        if invalid.returncode == 0:
            raise AssertionError("generator accepted an invalid context type")
        assert_contains(invalid.stderr, "tipo de context invalido", "invalid context type error")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
