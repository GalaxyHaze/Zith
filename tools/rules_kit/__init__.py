"""Shared Python primitives for Zith generators."""

from __future__ import annotations

from tools.rules_kit.errors import RuleError, render_error
from tools.rules_kit.text import (
    dedupe_preserve_order,
    parse_bool_flag,
    parse_hook,
    parse_string_list,
    parse_typed_member,
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

from tools.rules_kit.runtime import (
    gitignore_lines,
    write_generated,
)

__all__ = [
    "RuleError",
    "render_error",
    "dedupe_preserve_order",
    "parse_bool_flag",
    "parse_hook",
    "parse_string_list",
    "parse_typed_member",
    "gitignore_lines",
    "write_generated",
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
