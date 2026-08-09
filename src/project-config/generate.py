#!/usr/bin/env python3
"""Generate project-config.hpp/cpp and associated files from default.toml."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class ConfigError(ValueError):
    def __init__(self, path: Path, line_no: int, message: str) -> None:
        super().__init__(message)
        self.path = path
        self.line_no = line_no
        self.message = message

    def render(self) -> str:
        return f"{self.path}:{self.line_no}: {self.message}"


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def to_camel(name: str) -> str:
    parts = [part for part in re.split(r"[_-]", name) if part]
    if not parts:
        return name
    return parts[0] + "".join(part.capitalize() for part in parts[1:])


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


def parse_string_quoted(raw: str, line_no: int) -> str:
    raw = raw.strip()
    if not (
        (raw.startswith('"') and raw.endswith('"'))
        or (raw.startswith("'") and raw.endswith("'"))
    ):
        raise ConfigError(Path("<toml>"), line_no, f"string esperada: {raw!r}")
    body = raw[1:-1]
    try:
        return json.loads(f'"{body}"' if raw.startswith('"') else f'"{body}"')
    except (ValueError, SyntaxError) as exc:
        raise ConfigError(Path("<toml>"), line_no, f"string invalida: {raw!r}") from exc


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
        raise ConfigError(Path("<toml>"), line_no, "lista/string nao terminada")
    parts.append(body[start:].strip())
    return parts


def parse_strings(raw: str, line_no: int) -> list[str]:
    raw = raw.strip()
    if not (raw.startswith("[") and raw.endswith("]")):
        raise ConfigError(Path("<toml>"), line_no, f"lista de strings esperada: {raw!r}")
    inner = raw[1:-1]
    values: list[str] = []
    for item in split_top_level(inner, line_no):
        if item:
            values.append(parse_string_quoted(item, line_no))
    return values


def parse_toml_scalar(raw: str, line_no: int) -> bool | int | list[str] | str:
    raw = raw.strip()
    if raw.startswith('"') or raw.startswith("'"):
        return parse_string_quoted(raw, line_no)
    if raw.startswith("[") and raw.endswith("]"):
        return parse_strings(raw, line_no)
    lowered = raw.lower()
    if lowered in {"true", "false"}:
        return lowered == "true"
    if re.fullmatch(r"[+-]?\d+", raw):
        return int(raw)
    raise ConfigError(Path("<toml>"), line_no, f"valor TOML invalido: {raw!r}")


def parse_key_value(raw: str, line_no: int) -> tuple[str, bool | int | list[str] | str]:
    if "=" not in raw:
        raise ConfigError(Path("<toml>"), line_no, f"chave sem valor: {raw!r}")
    key, value_text = raw.split("=", 1)
    key = key.strip()
    value_text = value_text.strip()
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", key):
        raise ConfigError(Path("<toml>"), line_no, f"nome de chave invalido: {key!r}")
    if not value_text:
        raise ConfigError(Path("<toml>"), line_no, f"chave sem valor: {key!r}")
    value = parse_toml_scalar(value_text, line_no)
    return key, value


@dataclass(frozen=True)
class Field:
    section: str
    name: str
    value: Any
    line_no: int

    @property
    def qualified(self) -> str:
        return f"{self.section}.{self.name}"

    @property
    def member(self) -> str:
        return to_camel(self.name)

    @property
    def cpp_type(self) -> str:
        if isinstance(self.value, bool):
            return "bool"
        if isinstance(self.value, int):
            return "int"
        if isinstance(self.value, str):
            return "memory::InternedId"
        if isinstance(self.value, list):
            return "memory::DynArray<memory::InternedId>"
        raise ConfigError(Path("<toml>"), self.line_no,
                          f"tipo suportado apenas string/bool/int/array de strings: "
                          f"{self.qualified}")

    def emit_struct(self) -> str:
        return f"    {self.cpp_type} {self.member};"

    def emit_init(self) -> str:
        if isinstance(self.value, str):
            return f"{self.member}(strings.intern({cpp_string(self.value)}))"
        if isinstance(self.value, list):
            return f"{self.member}(arena)"
        return f"{self.member} = {self.default_expr}"

    @property
    def default_expr(self) -> str:
        if isinstance(self.value, bool):
            return "true" if self.value else "false"
        if isinstance(self.value, int):
            return str(self.value)
        if isinstance(self.value, str):
            return f"strings.intern({cpp_string(self.value)})"
        return "{}"


@dataclass
class Config:
    fields: list[Field]
    path: Path

    def fields_for(self, section: str) -> list[Field]:
        return [field for field in self.fields if field.section == section]

    def expected_sections(self) -> list[str]:
        return ["project", "build", "paths", "ffi"]

    def validate(self) -> None:
        seen: set[tuple[str, str]] = set()
        for field in self.fields:
            if field.section not in self.expected_sections():
                raise ConfigError(self.path, field.line_no,
                                  f"secao desconhecida: [{field.section}]")
            if (field.section, field.name) in seen:
                raise ConfigError(self.path, field.line_no,
                                  f"campo repetido: {field.qualified}")
            seen.add((field.section, field.name))
            if isinstance(field.value, list) and not all(
                isinstance(item, str) for item in field.value
            ):
                raise ConfigError(self.path, field.line_no,
                                  f"array de strings esperado: {field.qualified}")


def parse_toml(path: Path) -> Config:
    fields: list[Field] = []
    section = ""
    real_path = path.resolve()
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = strip_comment(raw)
        if not line:
            continue
        header = re.fullmatch(r"\[([^\]]+)\]", line)
        if header:
            section = header.group(1).strip()
            if not section:
                raise ConfigError(real_path, line_no, "secção vazia")
            if section not in {"project", "build", "paths", "ffi"}:
                raise ConfigError(real_path, line_no, f"secao desconhecida: [{section}]")
            continue
        if not section:
            raise ConfigError(real_path, line_no, "chave fora de secção")
        key, value = parse_key_value(line, line_no)
        fields.append(Field(section=section, name=key, value=value, line_no=line_no))
    return Config(fields=fields, path=real_path)


def emit_list_init_statements(config: Config, indent: str) -> list[str]:
    statements: list[str] = []
    for field in config.fields:
        if not isinstance(field.value, list):
            continue
        for item in field.value:
            statements.append(
                f"{indent}{field.member}.push(strings.intern({cpp_string(item)}));"
            )
    return statements


def make_header(config: Config) -> str:
    lines = [
        "#pragma once",
        "",
        '#include "common/dyn-array.hpp"',
        '#include "common/string-interner.hpp"',
        '#include "common/result.hpp"',
        "",
        "#include <string_view>",
        "",
        "namespace zith {",
        "",
        "struct ProjectConfig {",
    ]
    for section in config.expected_sections():
        section_fields = config.fields_for(section)
        if not section_fields:
            continue
        lines.append(f"    // [{section}]")
        lines.extend(field.emit_struct() for field in section_fields)
        lines.append("")
    lines.extend(
        [
            "    ProjectConfig(memory::Arena &arena, memory::StringInterner &strings)",
        ]
    )
    init_lines: list[str] = []
    body_lines: list[str] = ["        (void)arena;", "        (void)strings;"]
    body_lines.extend(emit_list_init_statements(config, "        "))
    for field in config.fields:
        if isinstance(field.value, (bool, int)):
            body_lines.append(f"        {field.member} = {field.default_expr};")
        elif isinstance(field.value, (str, list)):
            init_lines.append(field.emit_init())
    if init_lines:
        lines.append("        : " + ",\n          ".join(init_lines))
    lines.append("    {")
    lines.extend(body_lines)
    lines.extend(
        [
            "    }",
            "};",
            "",
            "memory::Result<void, memory::Error> loadFromToml(",
            "    std::string_view tomlText, memory::Arena &arena,",
            "    memory::StringInterner &strings, ProjectConfig &config);",
            "",
            "} // namespace zith",
        ]
    )
    return "\n".join(lines) + "\n"


def make_source(config: Config) -> str:
    lines = [
        '#include "project-config.hpp"',
        "",
        "#include <charconv>",
        "#include <string>",
        "#include <string_view>",
        "#include <system_error>",
        "#include <vector>",
        "",
        "namespace zith {",
        "",
        "namespace {",
        "",
        "bool parseBool(std::string_view value, bool &out) noexcept {",
        "    if (value == \"true\") { out = true; return true; }",
        "    if (value == \"false\") { out = false; return true; }",
        "    return false;",
        "}",
        "",
        "bool parseString(std::string_view value, std::string &out) noexcept {",
        "    if (value.size() < 2 || value.front() != '\"' || value.back() != '\"')",
        "        return false;",
        "    out.clear();",
        "    const std::string_view body = value.substr(1, value.size() - 2);",
        "    for (std::size_t i = 0; i < body.size(); ++i) {",
        "        if (body[i] == '\\\\') {",
        "            if (i + 1 >= body.size()) return false;",
        "            const char escaped = body[++i];",
        "            switch (escaped) {",
        "            case 'n': out.push_back('\\n'); break;",
        "            case 'r': out.push_back('\\r'); break;",
        "            case 't': out.push_back('\\t'); break;",
        "            case '\\\\': out.push_back('\\\\'); break;",
        "            case '\"': out.push_back('\"'); break;",
        "            default: return false;",
        "            }",
        "        } else {",
        "            out.push_back(body[i]);",
        "        }",
        "    }",
        "    return true;",
        "}",
        "",
        "bool parseInt(std::string_view value, int &out) noexcept {",
        "    const char *first = value.data();",
        "    const char *last = value.data() + value.size();",
        "    const auto result = std::from_chars(first, last, out, 10);",
        "    return result.ec == std::errc{} && result.ptr == last;",
        "}",
        "",
        "bool parseStringList(std::string_view value, std::vector<std::string> &out) noexcept {",
        "    if (value.size() < 2 || value.front() != '[' || value.back() != ']')",
        "        return false;",
        "    out.clear();",
        "    std::string_view inner = value.substr(1, value.size() - 2);",
        "    std::size_t cursor = 0;",
        "    while (cursor < inner.size()) {",
        "        while (cursor < inner.size() && (inner[cursor] == ' ' || inner[cursor] == '\\t'))",
        "            ++cursor;",
        "        if (cursor == inner.size()) break;",
        "        std::size_t at = cursor;",
        "        bool inString = false;",
        "        for (; at < inner.size(); ++at) {",
        "            const char c = inner[at];",
        "            if (c == '\"' && (at == cursor || inner[at - 1] != '\\\\'))",
        "                inString = !inString;",
        "            if (!inString && c == ',') break;",
        "        }",
        "        if (inString) return false;",
        "        std::size_t end = at;",
        "        while (end > cursor && (inner[end - 1] == ' ' || inner[end - 1] == '\\t'))",
        "            --end;",
        "        std::string itemText(inner.substr(cursor, end - cursor));",
        "        std::string item;",
        "        if (!parseString(itemText, item)) return false;",
        "        out.push_back(item);",
        "        if (at == inner.size()) return true;",
        "        cursor = at + 1;",
        "    }",
        "    return true;",
        "}",
        "",
        "std::string_view stripComment(std::string_view line) noexcept {",
        "    std::size_t start = 0;",
        "    while (start < line.size() && (line[start] == ' ' || line[start] == '\\t'))",
        "        ++start;",
        "    bool inString = false;",
        "    std::size_t end = line.size();",
        "    for (std::size_t i = start; i < end; ++i) {",
        "        const char c = line[i];",
        "        if (c == '\"' && (i == start || line[i - 1] != '\\\\'))",
        "            inString = !inString;",
        "        if (c == '#' && !inString) {",
        "            end = i;",
        "            break;",
        "        }",
        "    }",
        "    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\\t'))",
        "        --end;",
        "    return line.substr(start, end - start);",
        "}",
        "",
        "} // namespace",
        "",
        "memory::Result<void, memory::Error> loadFromToml(",
        "    std::string_view tomlText, memory::Arena &arena, memory::StringInterner &strings,",
        "    ProjectConfig &config) {",
        "    (void)arena;",
        "    std::size_t lineNo = 1;",
        "    std::string_view section;",
        "    std::size_t start = 0;",
        "    while (start <= tomlText.size()) {",
        "        const std::size_t at = tomlText.find('\\n', start);",
        "        const std::size_t end = at == std::string_view::npos ? tomlText.size() : at;",
        "        const std::string_view line = stripComment(tomlText.substr(start, end - start));",
        "        if (!line.empty()) {",
        "            if (line.front() == '[' && line.back() == ']') {",
        "                section = line.substr(1, line.size() - 2);",
        "                if (section != \"project\" && section != \"build\" &&",
        "                    section != \"paths\" && section != \"ffi\") {",
        "                    return memory::Error{std::string(\"unknown section \") +",
        "                                         std::string(section)};",
        "                }",
        "            } else {",
        "                const std::size_t eq = line.find('=');",
        "                if (eq == std::string_view::npos) {",
        "                    return memory::Error{std::string(\"expected '=' on line \") +",
        "                                         std::to_string(lineNo)};",
        "                }",
        "                std::string_view name = line.substr(0, eq);",
        "                std::string_view value = line.substr(eq + 1);",
        "                while (!name.empty() && (name.front() == ' ' || name.front() == '\\t'))",
        "                    name = name.substr(1);",
        "                while (!name.empty() && (name.back() == ' ' || name.back() == '\\t'))",
        "                    name = name.substr(0, name.size() - 1);",
        "                while (!value.empty() && (value.front() == ' ' || value.front() == '\\t'))",
        "                    value = value.substr(1);",
        "                while (!value.empty() && (value.back() == ' ' || value.back() == '\\t'))",
        "                    value = value.substr(0, value.size() - 1);",
        "                if (section.empty() || name.empty() || value.empty()) {",
        "                    return memory::Error{std::string(\"invalid line \") +",
        "                                         std::to_string(lineNo)};",
        "                }",
        "                bool handled = false;",
    ]

    for field in config.fields:
        lines.append(
            f'                if (section == {cpp_string(field.section)} && '
            f'name == {cpp_string(field.name)}) {{'
        )
        if isinstance(field.value, bool):
            lines.extend(
                [
                    "                    bool parsed = false;",
                    "                    if (!parseBool(value, parsed))",
                    "                        return memory::Error{\"invalid bool for \" + std::string(name)};",
                    f"                    config.{field.member} = parsed;",
                ]
            )
        elif isinstance(field.value, int):
            lines.extend(
                [
                    "                    int parsed = 0;",
                    "                    if (!parseInt(value, parsed))",
                    "                        return memory::Error{\"invalid int for \" + std::string(name)};",
                    f"                    config.{field.member} = parsed;",
                ]
            )
        elif isinstance(field.value, str):
            lines.extend(
                [
                    "                    std::string parsed;",
                    "                    if (!parseString(value, parsed))",
                    "                        return memory::Error{\"invalid string for \" + std::string(name)};",
                    f"                    config.{field.member} = strings.intern(parsed);",
                ]
            )
        else:
            lines.extend(
                [
                    "                    std::vector<std::string> parsed;",
                    "                    if (!parseStringList(value, parsed))",
                    "                        return memory::Error{\"invalid list for \" + std::string(name)};",
                    f"                    config.{field.member}.clear();",
                    "                    for (const std::string &item : parsed)",
                    f"                        config.{field.member}.push(strings.intern(item));",
                ]
            )
        lines.extend(["                    handled = true;", "                }"])

    lines.extend(
        [
            "                (void)handled;",
            "            }",
            "        }",
            "        if (at == std::string_view::npos) break;",
            "        start = at + 1;",
            "        ++lineNo;",
            "    }",
            "    return {};",
            "}",
            "",
            "} // namespace zith",
            "",
        ]
    )
    return "\n".join(lines) + "\n"


def make_gitignore() -> str:
    return "\n".join(
        ["project-config.hpp", "project-config.cpp", ".gitignore", ""]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="default.toml to read")
    parser.add_argument(
        "--out",
        type=Path,
        default=Path.cwd(),
        help="output directory for generated sources (default: current directory)",
    )
    args = parser.parse_args()

    config = parse_toml(args.input)
    config.validate()

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    (out / "project-config.hpp").write_text(make_header(config), encoding="utf-8")
    (out / "project-config.cpp").write_text(make_source(config), encoding="utf-8")
    (out / ".gitignore").write_text(make_gitignore(), encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ConfigError as exc:
        print(exc.render(), file=sys.stderr)
        raise SystemExit(1)
