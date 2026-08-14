#!/usr/bin/env python3
"""Generate facts.hpp and .gitignore from facts.rules."""

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

from tools.rules_kit import (  # noqa: E402
    RuleError,
    gitignore_lines,
    join_logical_lines,
    validate_identifier,
    write_generated as write_generated_files,
)

_HEADER_RE = re.compile(r"\[([^\]]+)\]")
_ENUM_SECTION_RE = re.compile(r"\[enum\.([^\]]+)\]", re.IGNORECASE)
_ENUM_TYPE_RE = re.compile(r"^enum\[([A-Za-z_][A-Za-z0-9_]*)\]$")
_ATTRIBUTE_RE = re.compile(r"^attribute\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$")
_SUPPORTED_TYPES = {"Bool", "Int"}
_CPP_TYPES = {
    "Bool": "int",
    "Int": "int",
}


@dataclass
class FactAttribute:
    name: str
    rule_type: str
    cpp_type: str
    line_no: int


@dataclass
class FactDomain:
    name: str
    attributes: list[FactAttribute] = field(default_factory=list)
    line_no: int = 0


@dataclass(frozen=True)
class FactEnum:
    name: str
    members: dict[str, int]
    line_no: int


class _RulesParser:
    def __init__(self) -> None:
        self.domains: list[FactDomain] = []
        self.domain_by_name: dict[str, FactDomain] = {}
        self.enums: dict[str, FactEnum] = {}
        self.current_enum_name: str | None = None
        self.current: FactDomain | None = None
        self.saw_content = False
        self.text: str = ""
        self.path = Path("facts.rules")

    def _start_enum(self, name: str, line_no: int) -> None:
        validate_identifier(name, line_no, "enum")
        if name in self.enums:
            raise RuleError(line_no, f"enum repetido: {name!r}")
        self.enums[name] = FactEnum(name=name, members={}, line_no=line_no)
        self.current_enum_name = name
        self.saw_content = True

    def _start_domain(self, name: str, line_no: int) -> None:
        validate_identifier(name, line_no, "dominio")
        if name in self.domain_by_name:
            raise RuleError(line_no, f"dominio repetido: {name!r}")
        current = FactDomain(name, line_no=line_no)
        self.domain_by_name[name] = current
        self.domains.append(current)
        self.current = current
        self.current_enum_name = None
        self.saw_content = True

    def _parse_enum_member(self, line: str, line_no: int) -> None:
        if self.current_enum_name is None:
            raise RuleError(line_no, "entrada de enum fora de [enum.Nome]")
        enum = self.enums[self.current_enum_name]
        name, sep, raw_value = line.partition("=")
        name = name.strip()
        raw_value = raw_value.strip()
        if not sep or not name or not raw_value:
            raise RuleError(
                line_no, f"entrada invalida em [enum.{enum.name}]: {line!r}"
            )
        validate_identifier(name, line_no, "membro de enum")
        try:
            value = int(raw_value)
        except ValueError as exc:
            raise RuleError(line_no, "valor de enum invalido") from exc
        if value < -(2**31) or value > 2**31 - 1:
            raise RuleError(line_no, "valor de enum tem de caber em std::int32_t")
        if name in enum.members:
            raise RuleError(line_no, f"membro de enum duplicado: {name!r}")
        if value in enum.members.values():
            raise RuleError(line_no, f"valor de enum duplicado: {value}")
        enum.members[name] = value
        self.saw_content = True

    def _parse_attribute(self, line: str, line_no: int) -> None:
        if self.current is None:
            raise RuleError(line_no, f"seccao [domain] esperada: {line!r}")
        attribute = _ATTRIBUTE_RE.fullmatch(line.strip())
        if not attribute:
            raise RuleError(line_no, f"campo invalido: {line!r}")
        name = attribute.group(1)
        rule_type = attribute.group(2).strip()
        enum_match = _ENUM_TYPE_RE.fullmatch(rule_type)
        if enum_match:
            enum_name = enum_match.group(1)
            if enum_name not in self.enums:
                raise RuleError(line_no, f"enum desconhecido: {enum_name!r}")
            rule_type = f"enum[{enum_name}]"
            cpp_type = "int"
        else:
            if rule_type not in _SUPPORTED_TYPES:
                raise RuleError(
                    line_no,
                    f"tipo invalido para atributo {name!r}: {rule_type!r} "
                    "(esperado Bool, Int ou enum[Nome])",
                )
            cpp_type = _CPP_TYPES[rule_type]
        if any(existing.name == name for existing in self.current.attributes):
            raise RuleError(line_no, f"atributo repetido: {name!r}")
        self.current.attributes.append(
            FactAttribute(
                name=name,
                rule_type=rule_type,
                cpp_type=cpp_type,
                line_no=line_no,
            )
        )
        self.saw_content = True

    def parse(self, text: str, path: Path) -> list[FactDomain]:
        self.text = text
        self.path = path
        for line_no, line in join_logical_lines(self.text):
            if line.startswith("[domain]"):
                name = line[line.rindex("]") + 1 :].strip()
                if not name:
                    raise RuleError(line_no, "[domain] tem de declarar um nome")
                self._start_domain(name, line_no)
                continue
            header = _HEADER_RE.fullmatch(line)
            if header:
                raw_section = header.group(1).strip()
                enum_match = _ENUM_SECTION_RE.fullmatch(line)
                if enum_match:
                    self._start_enum(enum_match.group(1).strip(), line_no)
                    continue
                raise RuleError(line_no, f"seccao desconhecida: [{raw_section}]")

            if self.current_enum_name is not None:
                self._parse_enum_member(line, line_no)
            else:
                self._parse_attribute(line, line_no)

        if not self.saw_content:
            raise RuleError(1, "facts.rules tem de declarar pelo menos um dominio")
        for enum in self.enums.values():
            if not enum.members:
                raise RuleError(
                    enum.line_no,
                    f"[enum.{enum.name}] tem de declarar pelo menos um membro",
                )
        for domain in self.domains:
            if not domain.attributes:
                raise RuleError(
                    domain.line_no,
                    f"[domain] {domain.name} tem de declarar pelo menos um atributo",
                )
        return self.domains


def parse_rules(text: str, path: Path) -> list[FactDomain]:
    return _RulesParser().parse(text, path)


def _enum_constant(name: str) -> str:
    return "".join(part[0].upper() + part[1:] for part in re.split(r"[_]", name) if part)


def _enum_field(name: str) -> str:
    return name


def _make_header(enums: dict[str, FactEnum], domains: list[FactDomain]) -> str:
    out: list[str] = []
    out.append("// auto-generated by src/facts/generate.py - do not edit by hand")
    out.append("#pragma once")
    out.append("")
    out.append('#include "facts/fact-v2.hpp"')
    out.append("#include <cstddef>")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace toolkit::facts {")
    out.append("")
    for enum in enums.values():
        out.append(f"enum class {enum.name} : std::int32_t {{")
        for index, (member_name, value) in enumerate(enum.members.items()):
            comma = "," if index + 1 < len(enum.members) else ""
            out.append(f"    {member_name} = {value}{comma}")
        out.append("};")
        out.append("")
        out.append(f"inline constexpr std::uint16_t k{enum.name}ValueCount =")
        out.append(f"    {len(enum.members)};")
        out.append("")
        out.append(f"inline constexpr {enum.name} k{enum.name}Values[] = {{")
        for index, member_name in enumerate(enum.members):
            comma = "," if index + 1 < len(enum.members) else ""
            out.append(f"    {enum.name}::{member_name}{comma}")
        out.append("};")
        out.append("")
    for domain in domains:
        out.append(f"enum class {domain.name}Attribute : std::uint16_t {{")
        for index, attribute in enumerate(domain.attributes):
            comma = "," if index + 1 < len(domain.attributes) else ""
            out.append(f"    {attribute.name} = {index}{comma}")
        out.append("};")
        out.append("")
        out.append(f"inline constexpr std::uint16_t k{domain.name}AttributeCount =")
        out.append(f"    {len(domain.attributes)};")
        out.append("")
        out.append(
            f"inline constexpr {domain.name}Attribute k{domain.name}Attributes[] = {{"
        )
        for index, attribute in enumerate(domain.attributes):
            comma = "," if index + 1 < len(domain.attributes) else ""
            out.append(f"    {domain.name}Attribute::{attribute.name}{comma}")
        out.append("};")
        out.append("")
        out.append(
            f"template <IntegralSigned Number> using {domain.name}FactStore = FactStore<Number>;"
        )
        out.append("")
        has_bool = any(attribute.rule_type == "Bool" for attribute in domain.attributes)
        has_int = any(attribute.rule_type == "Int" for attribute in domain.attributes)
        has_enum = any(
            attribute.rule_type.startswith("enum[") for attribute in domain.attributes
        )
        if has_bool:
            out.append(f"using {domain.name}BoolStore = FactStore<int>;")
        if has_int or has_enum:
            out.append(f"using {domain.name}IntStore = FactStore<int>;")
        if has_bool or has_int or has_enum:
            out.append("")
        for attribute in domain.attributes:
            out.append(
                f"using {domain.name}{_enum_constant(attribute.name)}Store = "
                f"FactStore<{attribute.cpp_type}>;"
            )
        out.append("")

    next_id = 0
    for domain in domains:
        for attribute in domain.attributes:
            constant = _enum_constant(attribute.name)
            out.append(
                f"inline constexpr FactId k{domain.name}{constant}Fact = FactId{{{next_id}}};"
            )
            next_id += 1
    out.append(f"inline constexpr std::uint16_t kGeneratedFactIdCount = {next_id};")
    out.append("")

    out.append("} // namespace toolkit::facts")
    out.append("")
    return "\n".join(out)


def _emit_gitignore() -> str:
    return gitignore_lines(["facts.hpp"])


def write_generated(
    out_dir: Path,
    enums: dict[str, FactEnum],
    domains: list[FactDomain],
) -> list[Path]:
    return write_generated_files(
        out_dir,
        [
            ("facts.hpp", _make_header(enums, domains)),
            (".gitignore", _emit_gitignore()),
        ],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rules", nargs="?", default="src/facts/facts.rules")
    parser.add_argument("--out", default="build/src/facts", help="diretorio de saida")
    args = parser.parse_args()

    rules_path = Path(args.rules)
    out_path = Path(args.out)
    if not rules_path.exists():
        print(f"rules file not found: {rules_path}", file=sys.stderr)
        return 2

    parser = _RulesParser()
    try:
        parser.parse(rules_path.read_text(encoding="utf-8"), rules_path)
    except RuleError as exc:
        print(exc.render(rules_path), file=sys.stderr)
        return 2

    write_generated(out_path, parser.enums, parser.domains)
    print(f"generated {out_path / 'facts.hpp'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
