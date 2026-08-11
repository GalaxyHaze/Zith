"""Errors raised by rules files."""

from __future__ import annotations

from pathlib import Path


class RuleError(ValueError):
    def __init__(self, line_no: int, message: str) -> None:
        super().__init__(message)
        self.line_no = line_no
        self.message = message

    def render(self, path: Path) -> str:
        return f"{path}:{self.line_no}: {self.message}"


def render_error(path: Path, exc: RuleError) -> str:
    return exc.render(path)
