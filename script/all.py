#!/usr/bin/env python3

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def run(*args: str) -> None:
    subprocess.run([sys.executable, *args], cwd=ROOT, check=True)


def main() -> int:
    run(
        str(ROOT / "src/cli/generate.py"),
        str(ROOT / "src/cli/cli.rules"),
        "--out",
        str(ROOT / "build/src/cli"),
    )
    run(
        str(ROOT / "src/frontend/lexer/generate.py"),
        str(ROOT / "src/frontend/lexer/lexer.rules"),
        "--out",
        str(ROOT / "build/src/frontend/lexer"),
        "--types",
        str(ROOT / "src/frontend/lexer/types.hpp"),
    )
    run(
        str(ROOT / "src/config/project/generate.py"),
        str(ROOT / "src/config/project/default.toml"),
        "--out",
        str(ROOT / "build/src/config/project"),
    )
    run(
        str(ROOT / "src/session/generate.py"),
        str(ROOT / "src/session/session.rules"),
        "--out",
        str(ROOT / "build/src/session"),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
