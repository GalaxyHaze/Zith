"""Shared generator entry-point and file-writing helpers."""

from __future__ import annotations

from pathlib import Path

__all__ = ["gitignore_lines", "write_generated"]


def gitignore_lines(files: list[str]) -> str:
    lines = [*files, ".gitignore", "__pycache__/"]
    return "\n".join(lines) + "\n"


def write_generated(out_dir: Path, files: list[tuple[str, str]]) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for name, content in files:
        target = out_dir / name
        target.write_text(content, encoding="utf-8")
        written.append(target)
    return written
