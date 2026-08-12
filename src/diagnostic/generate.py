#!/usr/bin/env python3
"""Generate error-info.hpp/cpp from error.rules."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve()
while not (_REPO_ROOT / "tools").is_dir():
    _REPO_ROOT = _REPO_ROOT.parent
sys.path.insert(0, str(_REPO_ROOT))

from tools.rules_kit import (
    RuleError,
    cpp_string,
    gitignore_lines,
    join_logical_lines,
    strip_comment,
    validate_identifier,
    write_generated as write_generated_files,
)

SEVERITIES = {"note", "warning", "error"}


@dataclass
class ErrorEntry:
    code: int
    code_name: str
    severity: str
    category: str
    title: str
    template: str
    note: str
    line_no: int

    @property
    def label(self) -> str:
        return f"E{self.code}"


def parse_rules(text: str, path: Path) -> list[ErrorEntry]:
    if not any(strip_comment(line) for line in text.splitlines()):
        raise RuleError(1, "error.rules tem de declarar pelo menos um erro")

    entries: list[ErrorEntry] = []
    seen_codes: set[int] = set()
    seen_names: set[str] = set()
    current: ErrorEntry | None = None
    section_seen = False

    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = strip_comment(raw)
        if not line:
            continue

        header = re.fullmatch(r"\[([^\]]+)\]", line)
        if header:
            section = header.group(1).strip().lower()
            if section != "errors":
                raise RuleError(line_no, f"seccao desconhecida: [{header.group(1)}]")
            if section_seen:
                raise RuleError(line_no, "seccao [errors] repetida")
            section_seen = True
            continue

        if not section_seen:
            raise RuleError(line_no, f"campo fora de [errors]: {line!r}")

        if ":" not in line:
            if current is None:
                raise RuleError(line_no, f"codigo esperado: {line!r}")
            name, sep, value = line.partition("=")
            if not sep:
                raise RuleError(line_no, f"campo invalido: {line!r}")
            name = name.strip().lower()
            value = value.strip()
            allowed = {"severity", "category", "title", "template", "note"}
            if name not in allowed:
                raise RuleError(line_no, f"campo desconhecido: {name!r}")
            if value.startswith('"') and value.endswith('"'):
                try:
                    value = _json_decode(value[1:-1])
                except ValueError as exc:
                    raise RuleError(line_no, f"string invalida: {exc}") from exc
            if name == "severity":
                if value.lower() not in SEVERITIES:
                    raise RuleError(line_no, f"severity invalida: {value!r}")
                current.severity = value.lower()
            elif name == "category":
                current.category = value
            elif name == "title":
                current.title = value
            elif name == "template":
                current.template = value
            elif name == "note":
                current.note = value
            continue

        code, fields_text = (part.strip() for part in line.split(":", 1))
        if not re.fullmatch(r"E\d{4}", code):
            raise RuleError(line_no, f"codigo invalido: {code!r}")
        code_value = int(code[1:])
        if code_value in seen_codes:
            raise RuleError(line_no, f"codigo repetido: {code!r}")
        name = fields_text.strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise RuleError(line_no, f"nome de erro invalido: {name!r}")
        base_name = re.sub(r"\d+$", "", re.sub(r"(?<!^)(?=[A-Z])", "_", name)).lower()
        code_name = f"E{code_value}_{base_name}" if base_name else f"E{code_value}"
        if code_name in seen_names:
            raise RuleError(line_no, f"nome de erro repetido: {name!r}")
        current = ErrorEntry(
            code=code_value,
            code_name=code_name,
            severity="error",
            category="compiler",
            title="",
            template="{message}",
            note="",
            line_no=line_no,
        )
        seen_codes.add(code_value)
        seen_names.add(code_name)
        entries.append(current)
        if fields_text:
            # The optional name field is accepted and retained for readability.
            validate_identifier(fields_text, line_no, "erro")

    if not entries:
        raise RuleError(1, "error.rules tem de declarar pelo menos um erro")
    for entry in entries:
        if entry.title and not entry.template:
            entry.template = entry.title
        if not entry.template:
            entry.template = "{message}"
    return entries


def _json_decode(body: str) -> str:
    import json

    return json.loads(f'"{body}"')


def make_header(entries: list[ErrorEntry]) -> str:
    lines = [
        "#pragma once",
        "",
        '#include "common/diagnostic/diagnostic.hpp"',
        "",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace common::diagnostic {",
        "",
        "struct ErrorInfo {",
        "    uint32_t code = 0;",
        "    Severity severity = Severity::Error;",
        "    std::string_view category;",
        "    std::string_view title;",
        "    std::string_view template_;",
        "    std::string_view note;",
        "};",
        "",
    ]
    for entry in entries:
        lines.append(f"inline constexpr ErrorInfo k{entry.code_name}{{")
        lines.append(f"    .code = {entry.code},")
        lines.append(f"    .severity = Severity::{entry.severity.capitalize()},")
        lines.append(f"    .category = {cpp_string(entry.category)},")
        lines.append(f"    .title = {cpp_string(entry.title)},")
        lines.append(f"    .template_ = {cpp_string(entry.template)},")
        lines.append(f"    .note = {cpp_string(entry.note)},")
        lines.append("};")
        lines.append("")
    lines.extend(
        [
            "[[nodiscard]] bool lookupError(uint32_t code, const ErrorInfo *&out) noexcept;",
            "[[nodiscard]] const ErrorInfo &errorInfo(uint32_t code) noexcept;",
            "",
            "struct ErrorTemplate {",
            "    const ErrorInfo *info = nullptr;",
            "",
            "    explicit ErrorTemplate(const ErrorInfo *value = nullptr) noexcept",
            "        : info(value) {}",
            "",
            "    [[nodiscard]] bool valid() const noexcept { return info != nullptr; }",
            "    [[nodiscard]] std::string render(",
            "        std::string_view message, std::string_view lexeme = {}) const;",
            "};",
            "",
            "} // namespace common::diagnostic",
            "",
        ]
    )
    return "\n".join(lines)


def make_source(entries: list[ErrorEntry]) -> str:
    lines = [
        '#include "diagnostic/error-info.hpp"',
        "",
        "#include <cstddef>",
        "#include <string>",
        "",
        "namespace common::diagnostic {",
        "namespace {",
        "",
        f"constexpr ErrorInfo kTable[] = {{",
    ]
    for entry in entries:
        lines.append(f"    k{entry.code_name},")
    lines.extend(
        [
            "};",
            "",
            "} // namespace",
            "",
            "bool lookupError(uint32_t code, const ErrorInfo *&out) noexcept {",
            "    for (const ErrorInfo &candidate : kTable) {",
            "        if (candidate.code == code) {",
            "            out = &candidate;",
            "            return true;",
            "        }",
            "    }",
            "    out = nullptr;",
            "    return false;",
            "}",
            "",
            "const ErrorInfo &errorInfo(uint32_t code) noexcept {",
            "    if (const ErrorInfo *info = nullptr; lookupError(code, info))",
            "        return *info;",
            "    static constexpr ErrorInfo kUnknown{",
            "        .code = 0,",
            "        .severity = Severity::Error,",
            '        .category = "compiler",',
            '        .title = "unknown error",',
            '        .template_ = "{message}",',
            '        .note = "",',
            "    };",
            "    return kUnknown;",
            "}",
            "",
            "std::string ErrorTemplate::render(",
            "    std::string_view message, std::string_view lexeme) const {",
            "    const std::string_view tpl = info ? info->template_ : std::string_view{\"{message}\"};",
            "    std::string out;",
            "    out.reserve(tpl.size() + message.size() + 16);",
            "    for (size_t i = 0; i < tpl.size(); ++i) {",
            "        if (tpl[i] == '{' && i + 1 < tpl.size()) {",
            "            const size_t close = tpl.find('}', i + 1);",
            "            if (close != std::string_view::npos) {",
            "                const std::string_view name = tpl.substr(i + 1, close - i - 1);",
                "                if (name == \"message\") {",
                "                    out.append(message);",
                "                    i = close;",
                "                    continue;",
                "                }",
                "                if (name == \"lexeme\") {",
                "                    out.append(lexeme);",
                "                    i = close;",
                "                    continue;",
                "                }",
            "            }",
            "        }",
            "        out.push_back(tpl[i]);",
            "    }",
            "    return out;",
            "}",
            "",
            "} // namespace common::diagnostic",
            "",
        ]
    )
    return "\n".join(lines)


def make_gitignore() -> str:
    return gitignore_lines(["error-info.hpp", "error-info.cpp"])


def write_generated(out_dir: Path, entries: list[ErrorEntry]) -> list[Path]:
    return write_generated_files(
        out_dir,
        [
            ("error-info.hpp", make_header(entries)),
            ("error-info.cpp", make_source(entries)),
            (".gitignore", make_gitignore()),
        ],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rules", nargs="?", default="src/diagnostic/error.rules")
    parser.add_argument("--out", default="build/src/diagnostic")
    args = parser.parse_args()

    rules_path = Path(args.rules)
    if not rules_path.exists():
        print(f"rules file not found: {rules_path}", file=sys.stderr)
        return 2
    try:
        entries = parse_rules(rules_path.read_text(encoding="utf-8"), rules_path)
    except RuleError as exc:
        print(exc.render(rules_path), file=sys.stderr)
        return 2

    written = write_generated(Path(args.out), entries)
    for target in written:
        print(f"generated {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
