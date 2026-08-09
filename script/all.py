#!/usr/bin/env python3

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def run(args: list[str]) -> None:
    subprocess.run([sys.executable, *args], cwd=ROOT, check=True)


def main() -> int:
    run([
        "src/cli/generate.py",
        "src/cli/cli.rules",
        "--out",
        "build/src/cli",
    ])
    run([
        "src/frontend/lexer/generate.py",
        "src/frontend/lexer/lexer.rules",
        "--out",
        "build/src/frontend/lexer",
        "--types",
        "src/frontend/lexer/types.hpp",
    ])
    run([
        "src/config/project/generate.py",
        "src/config/project/default.toml",
        "--out",
        "build/src/config/project",
    ])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
