"""Shared Python primitives for Zith generators."""

from __future__ import annotations

from tools.rules_kit.errors import RuleError, render_error
from tools.rules_kit.text import (
    cpp_char,
    cpp_string,
    is_balanced,
    join_logical_lines,
    parse_quoted,
    split_top_level,
    strip_comment,
    to_camel,
    validate_cpp_type,
    validate_identifier,
)

__all__ = [
    "RuleError",
    "render_error",
    "cpp_char",
    "cpp_string",
    "is_balanced",
    "join_logical_lines",
    "parse_quoted",
    "split_top_level",
    "strip_comment",
    "to_camel",
    "validate_cpp_type",
    "validate_identifier",
]
