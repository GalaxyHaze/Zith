#!/usr/bin/env python3

from __future__ import annotations

import sys
import tempfile
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.test_kit import assert_contains, expect_error, run_generator


def add_defaults(text: str) -> str:
    return (
        "[errors]\n"
        "E4001: generic\n"
        "severity = Error\n"
        "category = compiler\n"
        'title = "generic error"\n'
        'template = "{message}"\n'
        'note = "invalid source"\n'
        + text
    )


def main() -> int:
    repo_root = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="diagnostic-generator-", dir="/tmp") as tmp:
        tmpdir = Path(tmp)
        out_dir = tmpdir / "out"
        out_dir.mkdir(parents=True)
        rules = tmpdir / "error.rules"
        rules.write_text(
            add_defaults(
                "E4002: lookup\n"
                "severity = Error\n"
                "category = name\n"
                'title = "name lookup"\n'
                'template = "unknown {lexeme}"\n'
                'note = "check the spelling"\n'
            ),
            encoding="utf-8",
        )
        result = run_generator(
            repo_root,
            rules,
            out_dir,
            generator=repo_root / "src/diagnostic/generate.py",
        )
        if result.returncode != 0:
            raise AssertionError(result.stderr)
        header = (out_dir / "error-info.hpp").read_text(encoding="utf-8")
        source = (out_dir / "error-info.cpp").read_text(encoding="utf-8")
        assert_contains(header, "E4001", "error code constant")
        assert_contains(header, "E4002", "second error code constant")
        assert_contains(source, "lookupError", "lookup implementation")

        expect_error(
            repo_root,
            tmpdir,
            add_defaults("E4001: duplicate\n"),
            "codigo repetido",
            "duplicate error code",
            generator=repo_root / "src/diagnostic/generate.py",
            generator_kind=None,
        )
        expect_error(
            repo_root,
            tmpdir,
            add_defaults("E9999: bad\nseverity = Maybe\n"),
            "severity invalida",
            "invalid severity",
            generator=repo_root / "src/diagnostic/generate.py",
            generator_kind=None,
        )
        expect_error(
            repo_root,
            tmpdir,
            add_defaults("E9999: bad\nunknown = value\n"),
            "campo desconhecido",
            "unknown field",
            generator=repo_root / "src/diagnostic/generate.py",
            generator_kind=None,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
