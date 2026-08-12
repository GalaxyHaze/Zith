"""Text and syntax helpers shared by rules generators."""

from __future__ import annotations

import json
import re
from pathlib import Path

from tools.rules_kit.errors import RuleError

def dedupe_preserve_order(values: list[str]) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        out.append(value)
    return out


def parse_string_list(raw: str, line_no: int) -> list[str]:
    raw = raw.strip()
    if raw.startswith("[") and raw.endswith("]"):
        raw = raw[1:-1]
    values: list[str] = []
    for item in split_top_level(raw, line_no):
        if not item:
            continue
        values.append(parse_quoted(item, line_no))
    return values


def parse_bool_flag(raw: str, line_no: int, label: str) -> bool:
    lowered = raw.strip().lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    raise RuleError(line_no, f"{label} invalido: {raw!r}")


def strip_comment(line: str) -> str:
    quote: str | None = None
    escaped = False
    out: list[str] = []
    for ch in line:
        if quote:
            out.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = None
            continue
        if ch in {'"', "'"}:
            quote = ch
            out.append(ch)
            continue
        if ch == "#":
            break
        out.append(ch)
    return "".join(out).strip()


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def cpp_char(value: str) -> str:
    if len(value) != 1:
        raise ValueError(f"expected one char, got {value!r}")
    escaped = json.dumps(value, ensure_ascii=True)[1:-1]
    if escaped == "\\":
        return "'\\\\'"
    return f"'{escaped}'"


def split_top_level(body: str, line_no: int) -> list[str]:
    parts: list[str] = []
    start = 0
    depth = 0
    quote: str | None = None
    escaped = False
    for index, ch in enumerate(body):
        if quote:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = None
            continue
        if ch in {'"', "'"}:
            quote = ch
        elif ch in "[{(":
            depth += 1
        elif ch in "]})":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(body[start:index].strip())
            start = index + 1
    if quote or depth != 0:
        raise RuleError(line_no, "lista/string nao terminada")
    parts.append(body[start:].strip())
    return parts


def parse_quoted(raw: str, line_no: int) -> str:
    raw = raw.strip()
    if not (
        (raw.startswith('"') and raw.endswith('"'))
        or (raw.startswith("'") and raw.endswith("'"))
    ):
        raise RuleError(line_no, f"string esperada: {raw!r}")
    body = raw[1:-1]
    try:
        return json.loads(f'"{body}"' if raw.startswith('"') else f'"{body}"')
    except (ValueError, SyntaxError) as exc:
        raise RuleError(line_no, f"string invalida: {raw!r}") from exc


def to_camel(name: str) -> str:
    parts = [part for part in re.split(r"[_-]", name) if part]
    if not parts:
        return name
    return parts[0] + "".join(part.capitalize() for part in parts[1:])


_IDENTIFIER_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_CPP_TYPE_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_:<>,. *&\[\]]*")


def validate_identifier(value: str, line_no: int, label: str) -> None:
    if not _IDENTIFIER_RE.fullmatch(value):
        raise RuleError(line_no, f"nome de {label} invalido: {value!r}")


def validate_cpp_type(value: str, line_no: int, label: str) -> None:
    if not _CPP_TYPE_RE.fullmatch(value):
        raise RuleError(line_no, f"tipo de {label} invalido: {value!r}")


def parse_hook(raw: str, line_no: int, return_type: str = "void") -> str:
    match = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_:]*)\s*\(\)", raw.strip())
    if not match:
        raise RuleError(line_no, f"forma de action invalida: {raw!r}")
    return match.group(1)


def parse_typed_member(
    raw: str,
    line_no: int,
    label: str,
    *,
    requires_default: bool,
) -> tuple[str, str, str]:
    if requires_default and "=" not in raw:
        raise RuleError(line_no, f"member invalido (falta '='): {raw!r}")
    lhs, sep, default = raw.partition("=")
    lhs = lhs.strip()
    default = default.strip()
    if lhs.endswith(":"):
        lhs = lhs[:-1].strip()
    if ":" not in lhs:
        raise RuleError(line_no, f"member sem tipo: {raw!r}")
    name, cpp_type = (part.strip() for part in lhs.split(":", 1))
    validate_identifier(name, line_no, f"nome de {label} invalido")
    validate_cpp_type(cpp_type, line_no, label)
    if requires_default and not default:
        raise RuleError(line_no, f"default vazio para {label}: {name!r}")
    return name, cpp_type, default


def is_balanced(body: str) -> bool:
    depth = 0
    quote: str | None = None
    escaped = False
    for ch in body:
        if quote:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = None
            continue
        if ch in {'"', "'"}:
            quote = ch
        elif ch in "[({":
            depth += 1
        elif ch in "])}":
            depth -= 1
            if depth < 0:
                return False
    return not quote and depth == 0


def join_logical_lines(text: str) -> list[tuple[int, str]]:
    logical: list[tuple[int, str]] = []
    pending: str | None = None
    pending_line = 0

    def flush_pending() -> None:
        nonlocal pending
        if pending is not None:
            logical.append((pending_line, pending))
            pending = None

    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = strip_comment(raw)
        if not line:
            continue
        if re.fullmatch(r"\[([^\]]+)\]", line):
            flush_pending()
            logical.append((line_no, line))
            continue
        if pending is None:
            pending = line
            pending_line = line_no
            if is_balanced(pending):
                flush_pending()
        else:
            pending += " " + line
            if is_balanced(pending):
                flush_pending()
    flush_pending()
    return logical
