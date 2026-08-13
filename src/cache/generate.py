#!/usr/bin/env python3
"""Generate the generic section-cache C++ surface from cache.rules."""

from __future__ import annotations

import argparse
import collections
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
    cpp_string,
    gitignore_lines,
    join_logical_lines,
    parse_quoted,
    split_top_level,
    validate_cpp_type,
    validate_identifier,
    write_generated as write_generated_files,
)

_SECTION_HEADER_RE = re.compile(r"\[([^\]]+)\]")
_FIELD_LINE_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$")
_VECTOR_RE = re.compile(
    r"^vector\[([^\]]+)\](?:\s*max_items\s*=\s*(\d+))?$"
)
_ENUM_RE = re.compile(r"^enum\[([A-Za-z_][A-Za-z0-9_]*)\]$")
_SCALAR_TYPES = {
    "u8",
    "u32",
    "u64",
    "i64",
    "float64",
    "string",
    "bool",
}
_SCALAR_ALIASES = {
    "uint32": "u32",
}


@dataclass
class CacheRules:
    name: str
    magic: str
    version: int
    endian: str
    metadata_fields: list[tuple[str, str]] = field(default_factory=list)
    sections: list[str] = field(default_factory=list)
    length_type: str = "uint32"
    align: int = 8
    checksum: str = "fnv1a64"
    enums: dict[str, "EnumSpec"] = field(default_factory=dict)
    records: dict[str, "RecordSpec"] = field(default_factory=dict)
    section_specs: dict[str, "SectionSpec"] = field(default_factory=dict)


@dataclass
class EnumSpec:
    name: str
    cpp_type: str
    values: dict[str, int] = field(default_factory=dict)


@dataclass
class FieldSpec:
    name: str
    serialized_type: str
    kind: str
    cpp_type: str
    max_items: int | None = None
    element_kind: str | None = None
    element_cpp_type: str | None = None
    cpp_override: str | None = None


@dataclass
class RecordSpec:
    name: str
    cpp_type: str
    fields: list[FieldSpec] = field(default_factory=list)


@dataclass
class SectionSpec:
    name: str
    fields: list[FieldSpec] = field(default_factory=list)
    item: FieldSpec | None = None


def _split_field(raw: str, line_no: int) -> tuple[str, str, str]:
    match = _FIELD_LINE_RE.fullmatch(raw.strip())
    if not match:
        raise RuleError(line_no, f"campo invalido: {raw!r}")
    name, rest = match.group(1), match.group(2).strip()
    validate_identifier(name, line_no, "campo")
    return name, rest, rest


def _parse_type(
    raw: str,
    line_no: int,
    *,
    records: dict[str, RecordSpec],
    enums: dict[str, EnumSpec],
) -> tuple[FieldSpec | None, str]:
    raw = raw.strip()
    if raw in _SCALAR_ALIASES:
        raw = _SCALAR_ALIASES[raw]
    serialized = raw
    cpp_override: str | None = None
    if " -> " in raw:
        serialized, _, cpp_override = raw.partition(" -> ")
        serialized = serialized.strip()
        if serialized in _SCALAR_ALIASES:
            serialized = _SCALAR_ALIASES[serialized]
        cpp_override = cpp_override.strip()

    vector = _VECTOR_RE.fullmatch(serialized)
    if vector:
        element = vector.group(1).strip()
        max_items = 4096
        if vector.group(2) is not None:
            max_items = int(vector.group(2))
            if max_items <= 0:
                raise RuleError(line_no, "max_items tem de ser positivo")
        inner = _parse_type(
            element,
            line_no,
            records=records,
            enums=enums,
        )[0]
        if inner is None:
            raise RuleError(line_no, f"tipo desconhecido: {element!r}")
        return FieldSpec(
            name="",
            serialized_type=serialized,
            kind="vector",
            cpp_type=f"std::vector<{inner.cpp_type}>",
            max_items=max_items,
            element_kind=inner.kind,
            element_cpp_type=inner.cpp_type,
        ), "vector"

    enum_match = _ENUM_RE.fullmatch(serialized)
    if enum_match:
        enum_name = enum_match.group(1)
        enum_spec = enums.get(enum_name)
        if enum_spec is None:
            raise RuleError(line_no, f"enum desconhecido: {enum_name!r}")
        return FieldSpec(
            name="",
            serialized_type=serialized,
            kind="enum",
            cpp_type=enum_spec.cpp_type,
        ), "enum"

    scalar = serialized if serialized in _SCALAR_TYPES else None
    if scalar is not None:
        cpp_type = cpp_override or {
            "u8": "std::uint8_t",
            "u32": "std::uint32_t",
            "u64": "std::uint64_t",
            "i64": "std::int64_t",
            "float64": "double",
            "string": "std::string",
            "bool": "bool",
        }[scalar]
        validate_cpp_type(cpp_type, line_no, "tipo C++ correspondente")
        return FieldSpec(
            name="",
            serialized_type=serialized,
            kind="scalar",
            cpp_type=cpp_type,
            cpp_override=cpp_override,
        ), "scalar"

    record_spec = records.get(serialized)
    if record_spec is not None:
        if cpp_override is not None:
            raise RuleError(line_no, "record nao aceita mapeamento C++ manual")
        return FieldSpec(
            name="",
            serialized_type=serialized,
            kind="record",
            cpp_type=record_spec.cpp_type,
        ), "record"

    raise RuleError(line_no, f"tipo desconhecido: {serialized!r}")


def parse_rules(text: str, path: Path) -> CacheRules:
    section = ""
    section_kind = ""
    section_name = ""
    name: str | None = None
    magic: str | None = None
    version: int | None = None
    endian: str | None = None
    metadata: list[tuple[str, str]] = []
    sections: list[str] = []
    length_type = "uint32"
    align = 8
    checksum = "fnv1a64"
    enums: dict[str, EnumSpec] = {}
    records: dict[str, RecordSpec] = {}

    enum_values: dict[str, dict[str, int]] = collections.defaultdict(dict)
    enum_cpp: dict[str, str] = {}
    record_cpp: dict[str, str] = {}
    record_fields: dict[str, list[tuple[str, str]]] = collections.defaultdict(list)
    section_fields: dict[str, list[tuple[str, str]]] = collections.defaultdict(list)
    section_items: dict[str, tuple[str, str]] = {}

    for line_no, line in join_logical_lines(text):
        header = _SECTION_HEADER_RE.fullmatch(line)
        if header:
            raw_section = header.group(1).strip()
            section = raw_section.lower()
            section_kind = ""
            section_name = ""
            if section.startswith("enum."):
                section_kind = "enum"
                section_name = raw_section.split(".", 1)[1]
                validate_identifier(section_name, line_no, "enum")
                if section_name in enum_cpp or section_name in enum_values:
                    raise RuleError(line_no, f"enum repetido: {section_name!r}")
            elif section.startswith("record."):
                section_kind = "record"
                section_name = raw_section.split(".", 1)[1]
                validate_identifier(section_name, line_no, "record")
                if section_name in record_cpp or section_name in record_fields:
                    raise RuleError(line_no, f"record repetido: {section_name!r}")
            elif section.startswith("section."):
                section_kind = "section"
                section_name = raw_section.split(".", 1)[1]
                validate_identifier(section_name, line_no, "seccao")
                if section_name in section_fields or section_name in section_items:
                    raise RuleError(line_no, f"seccao declarada duas vezes: {section_name!r}")
                if section_name not in sections:
                    raise RuleError(
                        line_no,
                        f"[section.{section_name}] nao esta declarada em [sections]",
                    )
            elif section not in {"format", "metadata", "sections", "encoding"}:
                raise RuleError(line_no, f"secao desconhecida: [{raw_section}]")
            continue

        if section_kind == "enum":
            key, sep, value = line.partition("=")
            key = key.strip()
            value = value.strip()
            if not sep:
                raise RuleError(line_no, f"entrada invalida em [enum.{section_name}]: {line!r}")
            if key == "cpp":
                if not (value.startswith('"') and value.endswith('"')):
                    raise RuleError(line_no, "cpp de enum tem de ser uma string")
                if section_name in enum_cpp:
                    raise RuleError(line_no, f"cpp de enum repetido: {section_name!r}")
                cpp_type = parse_quoted(value, line_no)
                validate_cpp_type(cpp_type, line_no, "tipo C++ de enum")
                enum_cpp[section_name] = cpp_type
                continue
            validate_identifier(key, line_no, "valor de enum")
            try:
                enum_value = int(value)
            except ValueError as exc:
                raise RuleError(line_no, "valor de enum invalido") from exc
            if enum_value < 0 or enum_value > 255:
                raise RuleError(line_no, "valor de enum tem de estar entre 0 e 255")
            if any(value == enum_value for value in enum_values[section_name].values()):
                raise RuleError(line_no, f"valor de enum duplicado: {enum_value}")
            if key in enum_values[section_name]:
                raise RuleError(line_no, f"nome de enum duplicado: {key!r}")
            enum_values[section_name][key] = enum_value
            continue

        if section_kind == "record":
            if line.startswith("cpp ="):
                value = line.partition("=")[2].strip()
                if not (value.startswith('"') and value.endswith('"')):
                    raise RuleError(line_no, "cpp de record tem de ser uma string")
                if section_name in record_cpp:
                    raise RuleError(line_no, f"cpp de record repetido: {section_name!r}")
                cpp_type = parse_quoted(value, line_no)
                validate_cpp_type(cpp_type, line_no, "tipo C++ de record")
                record_cpp[section_name] = cpp_type
                continue
            field_name, field_type, _ = _split_field(line, line_no)
            if any(existing[0] == field_name for existing in record_fields[section_name]):
                raise RuleError(line_no, f"campo repetido: {field_name!r}")
            record_fields[section_name].append((field_name, field_type))
            continue

        if section_kind == "section":
            field_name, rest, _ = _split_field(line, line_no)
            if field_name not in {"items", "fields"}:
                raise RuleError(line_no, f"campo de secao invalido: {field_name!r}")
            if field_name == "items" and section_name in section_fields:
                raise RuleError(line_no, "seccao nao pode ter items e fields")
            if field_name == "fields" and section_name in section_items:
                raise RuleError(line_no, "seccao nao pode ter fields e items")
            if field_name == "items":
                section_items[section_name] = (line_no, rest)
            else:
                parsed: list[tuple[str, str]] = []
                if rest.strip() in {"", "[]"}:
                    parsed = []
                else:
                    body = rest.strip()
                    if body.startswith("[") and body.endswith("]"):
                        body = body[1:-1]
                    for piece in split_top_level(body, line_no):
                        piece = piece.strip()
                        if not piece:
                            continue
                        item_name, item_type, _ = _split_field(piece, line_no)
                        parsed.append((item_name, item_type))
                section_fields[section_name] = parsed
            continue

        key, sep, value = (line.partition("=") if "=" in line else line.partition(":"))
        if not sep:
            if section == "sections":
                section_name = line.strip()
                validate_identifier(section_name, line_no, "seccao")
                if section_name in sections:
                    raise RuleError(line_no, f"seccao repetida: {section_name!r}")
                sections.append(section_name)
                continue
            raise RuleError(line_no, f"entrada invalida: {line!r}")
        key = key.strip()
        value = value.strip()

        if section == "format":
            if key == "name":
                if not (value.startswith('"') and value.endswith('"')):
                    raise RuleError(line_no, "name tem de ser uma string")
                format_name = value[1:-1]
                if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", format_name):
                    raise RuleError(line_no, "nome de formato invalido")
                if name is not None:
                    raise RuleError(line_no, "formato declarado duas vezes")
                name = format_name
            elif key == "magic":
                if not (value.startswith('"') and value.endswith('"')):
                    raise RuleError(line_no, "magic tem de ser uma string")
                if magic is not None:
                    raise RuleError(line_no, "magic declarado duas vezes")
                magic = bytes(value[1:-1], "utf-8").decode("utf-8")
            elif key == "version":
                try:
                    parsed = int(value)
                except ValueError as exc:
                    raise RuleError(line_no, "versao invalida") from exc
                if parsed <= 0 or parsed > 65535:
                    raise RuleError(line_no, "versao tem de estar entre 1 e 65535")
                version = parsed
            elif key == "endian":
                if not (value.startswith('"') and value.endswith('"')):
                    raise RuleError(line_no, "endian tem de ser \"little\"")
                if value[1:-1] != "little":
                    raise RuleError(line_no, "endian tem de ser little")
                endian = value[1:-1]
            else:
                raise RuleError(line_no, f"campo desconhecido em [format]: {key!r}")
            continue

        if section == "metadata":
            validate_identifier(key, line_no, "campo de metadata")
            validate_cpp_type(value, line_no, "tipo de metadata")
            metadata.append((key, value))
            continue

        if section == "sections":
            validate_identifier(line.strip(), line_no, "seccao")
            if line.strip() in sections:
                raise RuleError(line_no, f"seccao repetida: {line.strip()!r}")
            sections.append(line.strip())
            continue

        if section == "encoding":
            if key == "length":
                if value != "uint32":
                    raise RuleError(line_no, "length tem de ser uint32")
                length_type = value
            elif key == "align":
                try:
                    align = int(value)
                except ValueError as exc:
                    raise RuleError(line_no, "align invalido") from exc
                if align <= 0 or align > 16 or (align & (align - 1)) != 0:
                    raise RuleError(line_no, "align tem de ser potencia de 2 entre 1 e 16")
            elif key == "checksum":
                if value != "fnv1a64":
                    raise RuleError(line_no, "checksum tem de ser fnv1a64")
                checksum = value
            else:
                raise RuleError(line_no, f"campo desconhecido em [encoding]: {key!r}")
            continue

        raise RuleError(line_no, f"secao desconhecida: [{section}]")

    if name is None or magic is None or version is None or endian is None:
        raise RuleError(1, "[format] tem de declarar name, magic, version e endian")
    if not metadata:
        raise RuleError(1, "[metadata] tem de declarar pelo menos um campo")
    if not sections:
        raise RuleError(1, "[sections] tem de declarar pelo menos uma seccao")

    for section_name in sections:
        if section_name not in section_fields and section_name not in section_items:
            raise RuleError(1, f"[section.{section_name}] nao foi declarado")

    for enum_name, cpp_type in enum_cpp.items():
        values = enum_values[enum_name]
        if not values:
            raise RuleError(1, f"[enum.{enum_name}] tem de declarar pelo menos um valor")
        enums[enum_name] = EnumSpec(enum_name, cpp_type, values)

    for record_name, cpp_type in record_cpp.items():
        fields = record_fields[record_name]
        if not fields:
            raise RuleError(1, f"[record.{record_name}] tem de declarar pelo menos um campo")
        records[record_name] = RecordSpec(record_name, cpp_type, [])

    for record_name, raw_fields in record_fields.items():
        parsed_fields: list[FieldSpec] = []
        for raw_name, raw_type in raw_fields:
            parsed, _ = _parse_type(
                raw_type,
                1,
                records=records,
                enums=enums,
            )
            assert parsed is not None
            parsed.name = raw_name
            parsed_fields.append(parsed)
        records[record_name].fields = parsed_fields

    record_name_by_cpp = {spec.cpp_type: spec.name for spec in records.values()}
    visited: set[str] = set()
    visiting: set[str] = set()

    def visit_record(record_name: str, owners: list[str]) -> None:
        if record_name in visiting:
            chain = " -> ".join([*owners, record_name])
            raise RuleError(1, f"referencia circular em records: {chain}")
        if record_name in visited:
            return
        visiting.add(record_name)
        owners.append(record_name)
        for parsed in records[record_name].fields:
            if parsed.kind != "record":
                continue
            dep_name = record_name_by_cpp.get(parsed.cpp_type)
            if dep_name is None:
                raise RuleError(
                    1,
                    f"tipo de record referenciado ausente: {parsed.cpp_type!r}",
                )
            visit_record(dep_name, owners)
        owners.pop()
        visiting.remove(record_name)
        visited.add(record_name)

    for record_name in list(records):
        visit_record(record_name, [])

    section_specs: dict[str, SectionSpec] = {}
    for section_name in sections:
        fields: list[FieldSpec] = []
        item_spec: FieldSpec | None = None
        if section_name in section_fields:
            parsed_fields: list[FieldSpec] = []
            for raw_name, raw_type in section_fields[section_name]:
                parsed, _ = _parse_type(
                    raw_type,
                    1,
                    records=records,
                    enums=enums,
                )
                assert parsed is not None
                parsed.name = raw_name
                parsed_fields.append(parsed)
            fields = parsed_fields
        elif section_name in section_items:
            item_name, item_type = section_items[section_name]
            parsed, _ = _parse_type(
                item_type,
                1,
                records=records,
                enums=enums,
            )
            assert parsed is not None
            item_spec = parsed
        section_specs[section_name] = SectionSpec(
            section_name,
            fields=fields,
            item=item_spec,
        )

    return CacheRules(
        name=name,
        magic=magic,
        version=version,
        endian=endian,
        metadata_fields=metadata,
        sections=sections,
        length_type=length_type,
        align=align,
        checksum=checksum,
        enums=enums,
        records=records,
        section_specs=section_specs,
    )


def metadata_struct(rules: CacheRules) -> list[str]:
    lines = ["struct CacheMetadata {"]
    for field_name, cpp_type in rules.metadata_fields:
        lines.append(f"    {cpp_type} {field_name} = 0;")
    lines.append("};")
    return lines


def section_entries(rules: CacheRules) -> list[str]:
    lines = [
        "struct SectionEntry {",
        "    std::uint64_t offset = 0;",
        "    std::uint64_t size = 0;",
        "};",
        "",
        "enum class SectionId : std::uint8_t {",
    ]
    for index, section in enumerate(rules.sections):
        end = "," if index + 1 < len(rules.sections) else ""
        lines.append(f"    {section}{end}")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr std::size_t kSectionCount = " +
                 str(len(rules.sections)) + ";")
    lines.append("inline constexpr std::size_t kCacheFormatVersion = " +
                 str(rules.version) + ";")
    lines.append("inline constexpr std::size_t kCacheHeaderSize = 64 + " +
                 "16 * kSectionCount;")
    return lines


def header_lines(rules: CacheRules) -> list[str]:
    lines = [
        "#pragma once",
        "",
        '#include "cache/cache-error.hpp"',
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace toolkit::cache {",
        "",
    ]
    lines.extend(metadata_struct(rules))
    lines.append("")
    lines.extend(section_entries(rules))
    lines.extend(
        [
            "",
            "struct CacheHeader {",
            f"    inline static constexpr std::string_view kMagic = {cpp_string(rules.magic)};",
            f"    inline static constexpr std::uint32_t kVersion = {rules.version};",
            "    inline static constexpr std::uint8_t kEndian = 0x01;",
            "",
            "    std::uint8_t magic[8] = {};",
            "    std::uint32_t version = 0;",
            "    std::uint8_t endian = 0;",
            "    std::uint8_t reserved[3] = {};",
            "    std::uint32_t section_count = 0;",
            "    std::uint32_t header_size = 0;",
            "    std::uint64_t checksum = 0;",
            "    CacheMetadata metadata{};",
            "    SectionEntry entries[kSectionCount] = {};",
            "};",
            "",
            "struct CacheFile {",
            "    CacheHeader header{};",
            "    std::string_view payload;",
            "",
            "    [[nodiscard]] std::string_view section(SectionId id) const noexcept;",
            "    [[nodiscard]] bool valid() const noexcept;",
            "};",
            "",
            "} // namespace toolkit::cache",
            "",
        ]
    )
    return lines


def source_lines(rules: CacheRules) -> list[str]:
    lines = [
        '#include "cache/cache-section.hpp"',
        "",
        "namespace toolkit::cache {",
        "",
        "std::string_view CacheFile::section(SectionId id) const noexcept {",
        "    const std::size_t index = static_cast<std::size_t>(id);",
        "    if (index >= kSectionCount)",
        "        return {};",
        "    const auto &entry = header.entries[index];",
        "    if (entry.offset > payload.size() || entry.size > payload.size() - entry.offset)",
        "        return {};",
        "    return payload.substr(entry.offset, entry.size);",
        "}",
        "",
        "bool CacheFile::valid() const noexcept {",
        "    if (std::string_view(reinterpret_cast<const char *>(&header.magic[0]), "
        "CacheHeader::kMagic.size()) != CacheHeader::kMagic)",
        "        return false;",
        "    if (header.version != CacheHeader::kVersion || header.endian != CacheHeader::kEndian)",
        "        return false;",
        "    if (header.section_count != kSectionCount)",
        "        return false;",
        "    if (header.header_size != kCacheHeaderSize)",
        "        return false;",
        "    for (std::size_t i = 0; i < kSectionCount; ++i) {",
        "        const auto &entry = header.entries[i];",
        "        if (entry.offset > payload.size() || entry.size > payload.size() - entry.offset)",
        "            return false;",
        "        if (i + 1 < kSectionCount && entry.offset > header.entries[i + 1].offset)",
        "            return false;",
        "    }",
        "    return true;",
        "}",
        "",
        "} // namespace toolkit::cache",
        "",
    ]
    return lines


def _field_reader_temp(field: FieldSpec) -> str:
    if field.kind == "enum":
        return f"std::uint8_t {field.name}_raw = 0;"
    if field.kind == "scalar" and field.serialized_type == "bool":
        return f"std::uint8_t {field.name}_raw = 0;"
    if field.kind == "scalar" and field.cpp_override is not None:
        return f"std::int64_t {field.name}_parsed = 0;"
    if field.kind == "record":
        return f"{field.cpp_type} {field.name}_parsed;"
    return ""


def _field_after_read(field: FieldSpec, record_cpp: str) -> list[str]:
    if field.kind == "enum":
        enum_name = _enum_function_suffix(field.cpp_type)
        return [
            f"    const auto {field.name} = static_cast<{field.cpp_type}>({field.name}_raw);",
            f"    bool {field.name}_known = false;",
            f"    for (const auto candidate : kEnum{enum_name})",
            f"        {field.name}_known = {field.name}_known || candidate == {field.name};",
            f"    if (!{field.name}_known)",
            "        return false;",
            f"    {record_cpp}.{field.name} = {field.name};",
        ]
    if field.kind == "scalar" and field.serialized_type == "bool":
        return [
            f"    if ({field.name}_raw > 1)",
            "        return false;",
            f"    {record_cpp}.{field.name} = {field.name}_raw != 0;",
        ]
    if field.kind == "scalar" and field.cpp_override is not None:
        return [
            f"    if ({field.name}_parsed < std::numeric_limits<std::int32_t>::min() || "
            f"{field.name}_parsed > std::numeric_limits<std::int32_t>::max())",
            "        return false;",
            f"    {record_cpp}.{field.name} = static_cast<std::int32_t>({field.name}_parsed);",
        ]
    if field.kind == "record":
        return [
            f"    {record_cpp}.{field.name} = std::move({field.name}_parsed);",
        ]
    return []


def _field_read_call(field: FieldSpec, record_cpp: str) -> str:
    if field.kind == "scalar":
        if field.serialized_type == "string":
            return f"reader.readString({record_cpp}.{field.name})"
        if field.serialized_type == "bool":
            return f"reader.readU8({field.name}_raw)"
        if field.cpp_override is not None:
            return f"reader.readI64({field.name}_parsed)"
        return _scalar_read(field.serialized_type, f"{record_cpp}.{field.name}")
    if field.kind == "enum":
        return f"reader.readU8({field.name}_raw)"
    if field.kind == "vector":
        if field.element_kind == "record":
            return f"read{_vector_list_suffix(field)}List(reader, {record_cpp}.{field.name}, {field.max_items})"
        if field.element_kind == "enum":
            return f"readEnumList(reader, {record_cpp}.{field.name}, {field.max_items}, kEnum{_enum_function_suffix(field.element_cpp_type or '')})"
        return f"read{_vector_list_suffix(field)}List(reader, {record_cpp}.{field.name}, {field.max_items})"
    if field.kind == "record":
        return f"deserialize{_record_function_suffix(field.cpp_type)}(reader, {field.name}_parsed)"
    raise AssertionError(f"unsupported kind: {field.kind}")


def _field_write_call(field: FieldSpec, record_expr: str) -> str:
    if field.kind == "scalar":
        if field.serialized_type == "string":
            return f"writeString(writer, {record_expr}.{field.name})"
        if field.serialized_type == "bool":
            return f"writer.writeU8({record_expr}.{field.name} ? 1 : 0)"
        if field.cpp_override is not None:
            return f"writer.writeI64(static_cast<std::int64_t>({record_expr}.{field.name}))"
        return _scalar_write(field.serialized_type, f"{record_expr}.{field.name}")
    if field.kind == "enum":
        return f"writer.writeU8(static_cast<std::uint8_t>({record_expr}.{field.name}))"
    if field.kind == "vector":
        if field.element_kind == "record":
            return f"write{_vector_list_suffix(field)}List(writer, {record_expr}.{field.name})"
        if field.element_kind == "enum":
            return f"writeEnumList(writer, {record_expr}.{field.name}, kEnum{_enum_function_suffix(field.element_cpp_type or '')})"
        return f"write{_vector_list_suffix(field)}List(writer, {record_expr}.{field.name})"
    if field.kind == "record":
        return f"serialize{_record_function_suffix(field.cpp_type)}(writer, {record_expr}.{field.name})"
    raise AssertionError(f"unsupported kind: {field.kind}")


def _scalar_read(serialized_type: str, target: str) -> str:
    method = {
        "u8": "readU8",
        "u32": "readU32",
        "u64": "readU64",
        "i64": "readI64",
        "float64": "readDouble",
    }[serialized_type]
    return f"reader.{method}({target})"


def _scalar_write(serialized_type: str, value: str) -> str:
    method = {
        "u8": "writeU8",
        "u32": "writeU32",
        "u64": "writeU64",
        "i64": "writeI64",
        "float64": "writeDouble",
    }[serialized_type]
    return f"writer.{method}({value})"


def _vector_list_suffix(field: FieldSpec) -> str:
    assert field.element_kind is not None
    assert field.element_cpp_type is not None
    if field.element_kind == "record":
        return _record_function_suffix(field.element_cpp_type)
    if field.element_kind == "enum":
        return _enum_function_suffix(field.element_cpp_type)
    element = _vector_element_serialized(field)
    element = _SCALAR_ALIASES.get(element, element)
    if element == "string":
        return "String"
    return {
        "u8": "U8",
        "u32": "U32",
        "u64": "U64",
        "i64": "I64",
        "float64": "Double",
        "bool": "Bool",
    }[element]


def _record_function_suffix(cpp_type: str) -> str:
    short = cpp_type.split("::")[-1]
    if short.endswith("Record"):
        short = short[: -len("Record")]
    if short == "Dependency":
        return "Dependency"
    return short


def _enum_function_suffix(cpp_type: str) -> str:
    return cpp_type.split("::")[-1]


def _vector_element_serialized(field: FieldSpec) -> str:
    if field.element_kind == "enum":
        return f"enum[{_enum_function_suffix(field.element_cpp_type or '')}]"
    if field.element_kind == "record":
        return field.element_cpp_type.split("::")[-1]
    if field.element_kind == "scalar":
        # Recover the scalar token from the serialized vector text.
        match = re.fullmatch(r"vector\[([^\]]+)\].*", field.serialized_type)
        if match:
            return match.group(1)
    return ""


def _cpp_type_to_record_name(cpp_type: str, records: dict[str, RecordSpec]) -> str:
    for name, spec in records.items():
        if spec.cpp_type == cpp_type:
            return name
    raise KeyError(cpp_type)


def _vector_element_record_name(field: FieldSpec, records: dict[str, RecordSpec]) -> str:
    assert field.element_cpp_type is not None
    return _cpp_type_to_record_name(field.element_cpp_type, records)


def _typename(field: FieldSpec) -> str:
    return field.cpp_type


def codec_header_lines(rules: CacheRules) -> list[str]:
    lines = [
        "#pragma once",
        "",
        '#include "cache/cache-buffer.hpp"',
        '#include "cache/cache-types.hpp"',
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <array>",
        "#include <string>",
        "#include <vector>",
        "",
        "namespace toolkit::cache {",
        "",
    ]
    for enum_spec in rules.enums.values():
        lines.append(
            f"inline constexpr std::array<{enum_spec.cpp_type}, "
            f"{len(enum_spec.values)}> kEnum{enum_spec.name} = {{"
        )
        for name, value in enum_spec.values.items():
            lines.append(f"    {enum_spec.cpp_type}::{name},")
        lines.append("};")
        lines.append("")
    for record_spec in rules.records.values():
        function_name = _record_function_suffix(record_spec.cpp_type)
        lines.append(
            f"void serialize{function_name}(ByteWriter &writer, "
            f"const {record_spec.cpp_type} &value);"
        )
        lines.append(
            f"bool deserialize{function_name}(ByteReader &reader, "
            f"{record_spec.cpp_type} &value);"
        )
        lines.append("")
    for section in rules.sections:
        lines.append(
            f"void write{section}(ByteWriter &writer, const Artifact &artifact);"
        )
        lines.append(
            f"bool read{section}(ByteReader &reader, Artifact &artifact);"
        )
        lines.append("")
    lines.append("} // namespace toolkit::cache")
    lines.append("")
    return lines


def _scalar_list_code(serialized_type: str, cpp_element_type: str) -> list[str]:
    read_method = {
        "u8": "readU8",
        "u32": "readU32",
        "u64": "readU64",
        "i64": "readI64",
        "float64": "readDouble",
    }[serialized_type]
    write_method = {
        "u8": "writeU8",
        "u32": "writeU32",
        "u64": "writeU64",
        "i64": "writeI64",
        "float64": "writeDouble",
    }[serialized_type]
    suffix = _scalar_element_suffix(serialized_type)
    return [
        f"void write{suffix}List(ByteWriter &writer, const std::vector<{cpp_element_type}> &values) {{",
        "    if (values.size() > std::numeric_limits<std::uint32_t>::max()) {",
        "        writer.writeU32(0);",
        "        return;",
        "    }",
        "    writer.writeU32(static_cast<std::uint32_t>(values.size()));",
        f"    for (const {cpp_element_type} value : values)",
        f"        writer.{write_method}(static_cast<{cpp_element_type}>(value));",
        "}",
        "",
        f"bool read{suffix}List(ByteReader &reader, std::vector<{cpp_element_type}> &values, "
        "std::uint32_t max_items) {",
        "    std::uint32_t count = 0;",
        "    if (!reader.readU32(count) || count > max_items)",
        "        return false;",
        "    values.reserve(count);",
        "    for (std::uint32_t i = 0; i < count; ++i) {",
        f"        {cpp_element_type} value{{}};",
        f"        if (!reader.{read_method}(value))",
        "            return false;",
        "        values.push_back(value);",
        "    }",
        "    return true;",
        "}",
        "",
    ]


def _scalar_element_suffix(serialized_type: str) -> str:
    return {
        "u8": "U8",
        "u32": "U32",
        "u64": "U64",
        "i64": "I64",
        "float64": "Double",
        "string": "String",
        "bool": "Bool",
    }[serialized_type]


def _string_list_code() -> list[str]:
    return [
        "void writeStringList(ByteWriter &writer, const std::vector<std::string> &values) {",
        "    if (values.size() > std::numeric_limits<std::uint32_t>::max()) {",
        "        writer.writeU32(0);",
        "        return;",
        "    }",
        "    writer.writeU32(static_cast<std::uint32_t>(values.size()));",
        "    for (const auto &value : values)",
        "        writeString(writer, value);",
        "}",
        "",
        "bool readStringList(ByteReader &reader, std::vector<std::string> &values, "
        "std::uint32_t max_items) {",
        "    std::uint32_t count = 0;",
        "    if (!reader.readU32(count) || count > max_items)",
        "        return false;",
        "    values.reserve(count);",
        "    for (std::uint32_t i = 0; i < count; ++i) {",
        "        std::string value;",
        "        if (!readString(reader, value))",
        "            return false;",
        "        values.push_back(std::move(value));",
        "    }",
        "    return true;",
        "}",
        "",
    ]


def codec_source_lines(rules: CacheRules) -> list[str]:
    lines = [
        '#include "cache/cache-codec.gen.hpp"',
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <limits>",
        "#include <string>",
        "#include <utility>",
        "#include <vector>",
        "",
        "namespace toolkit::cache {",
        "",
        "bool fit32(std::size_t value) noexcept {",
        "    return value <= std::numeric_limits<std::uint32_t>::max();",
        "}",
        "",
        "void writeString(ByteWriter &writer, std::string_view value) {",
        "    writer.writeString(value);",
        "}",
        "",
        "bool readString(ByteReader &reader, std::string &value) {",
        "    return reader.readString(value);",
        "}",
        "",
    ]
    lines.extend(_string_list_code())

    for enum_spec in rules.enums.values():
        lines.extend(_enum_list_code(enum_spec))
    for serialized_type, cpp_element_type in [
        ("u8", "std::uint8_t"),
        ("u32", "std::uint32_t"),
        ("u64", "std::uint64_t"),
        ("i64", "std::int64_t"),
        ("float64", "double"),
    ]:
        lines.extend(_scalar_list_code(serialized_type, cpp_element_type))

    record_list_fields: dict[str, FieldSpec] = {}
    for record_spec in rules.records.values():
        for field in record_spec.fields:
            if field.kind != "vector" or field.element_kind != "record":
                continue
            suffix = _vector_list_suffix(field)
            record_list_fields[suffix] = field
    for section_spec in rules.section_specs.values():
        if section_spec.item is not None and section_spec.item.element_kind == "record":
            field = section_spec.item
            suffix = _vector_list_suffix(field)
            record_list_fields[suffix] = field
        for field in section_spec.fields:
            if field.kind == "vector" and field.element_kind == "record":
                suffix = _vector_list_suffix(field)
                record_list_fields[suffix] = field

    generated_record_lists: set[str] = set()
    for suffix, field in record_list_fields.items():
        if suffix in generated_record_lists:
            continue
        generated_record_lists.add(suffix)
        lines.extend(_record_vector_code(rules, field))

    for record_spec in rules.records.values():
        lines.extend(_record_code(rules, record_spec))

    for section_name in rules.sections:
        lines.extend(_section_code(rules, section_name))

    lines.extend(["} // namespace toolkit::cache", ""])
    return lines


def _enum_list_code(enum_spec: EnumSpec) -> list[str]:
    enum_suffix = _enum_function_suffix(enum_spec.cpp_type)
    count = len(enum_spec.values)
    return [
        f"void write{enum_suffix}List(ByteWriter &writer, "
        f"const std::vector<{enum_spec.cpp_type}> &values, "
        f"const std::array<{enum_spec.cpp_type}, {count}> &allowed) {{",
        "    if (values.size() > std::numeric_limits<std::uint32_t>::max()) {",
        "        writer.writeU32(0);",
        "        return;",
        "    }",
        "    writer.writeU32(static_cast<std::uint32_t>(values.size()));",
        "    for (const auto value : values)",
        "        writer.writeU8(static_cast<std::uint8_t>(value));",
        "}",
        "",
        f"bool read{enum_suffix}List(ByteReader &reader, "
        f"std::vector<{enum_spec.cpp_type}> &values, "
        f"std::uint32_t max_items, "
        f"const std::array<{enum_spec.cpp_type}, {count}> &allowed) {{",
        "    std::uint32_t count = 0;",
        "    if (!reader.readU32(count) || count > max_items)",
        "        return false;",
        "    values.reserve(count);",
        "    for (std::uint32_t i = 0; i < count; ++i) {",
        "        std::uint8_t raw = 0;",
        "        if (!reader.readU8(raw))",
        "            return false;",
        f"        const auto value = static_cast<{enum_spec.cpp_type}>(raw);",
        "        bool known = false;",
        "        for (const auto candidate : allowed)",
        "            known = known || candidate == value;",
        "        if (!known)",
        "            return false;",
        "        values.push_back(value);",
        "    }",
        "    return true;",
        "}",
        "",
    ]


def _record_code(rules: CacheRules, record_spec: RecordSpec) -> list[str]:
    function = _record_function_suffix(record_spec.cpp_type)
    fields = record_spec.fields
    write_lines = [
        f"void serialize{function}(ByteWriter &writer, "
        f"const {record_spec.cpp_type} &value) {{"
    ]
    for field in fields:
        write_lines.append(f"    {_field_write_call(field, 'value')};")
    write_lines.append("}")
    write_lines.append("")

    read_lines = [
        f"bool deserialize{function}(ByteReader &reader, "
        f"{record_spec.cpp_type} &value) {{"
    ]
    temps = [_field_reader_temp(field) for field in fields if _field_reader_temp(field)]
    for temp in temps:
        read_lines.append(f"    {temp}")
    post = []
    for field in fields:
        post.extend(_field_after_read(field, "value"))
    calls = [_field_read_call(field, "value") for field in fields]
    if calls:
        read_lines.append("    const bool ok = " + " &&\n           ".join(calls) + ";")
        read_lines.append("    if (!ok)")
        read_lines.append("        return false;")
        read_lines.extend(post)
        read_lines.append("    return true;")
    else:
        read_lines.append("    return true;")
    read_lines.append("}")
    read_lines.append("")

    return [*write_lines, *read_lines]


def _record_vector_code(rules: CacheRules, field: FieldSpec) -> list[str]:
    assert field.element_kind == "record"
    assert field.element_cpp_type is not None
    suffix = _vector_list_suffix(field)
    return [
        f"void write{suffix}List(ByteWriter &writer, const {field.cpp_type} &values) {{",
        "    if (values.size() > std::numeric_limits<std::uint32_t>::max()) {",
        "        writer.writeU32(0);",
        "        return;",
        "    }",
        "    writer.writeU32(static_cast<std::uint32_t>(values.size()));",
        f"    for (const {field.element_cpp_type} &value : values)",
        f"        serialize{_record_function_suffix(field.element_cpp_type)}(writer, value);",
        "}",
        "",
        f"bool read{suffix}List(ByteReader &reader, {field.cpp_type} &values, "
        "std::uint32_t max_items) {",
        "    std::uint32_t count = 0;",
        "    if (!reader.readU32(count) || count > max_items)",
        "        return false;",
        "    values.reserve(count);",
        "    for (std::uint32_t i = 0; i < count; ++i) {",
        f"        {field.element_cpp_type} value{{}};",
        f"        if (!deserialize{_record_function_suffix(field.element_cpp_type)}(reader, value))",
        "            return false;",
        "        values.push_back(std::move(value));",
        "    }",
        "    return true;",
        "}",
        "",
    ]


def _section_code(rules: CacheRules, section_name: str) -> list[str]:
    spec = rules.section_specs[section_name]
    write_lines = [
        f"void write{section_name}(ByteWriter &writer, const Artifact &artifact) {{"
    ]
    read_lines = [
        f"bool read{section_name}(ByteReader &reader, Artifact &artifact) {{"
    ]
    for field in spec.fields:
        write_lines.append(f"    {_field_write_call(field, 'artifact')};")
    if spec.item is not None:
        item = spec.item
        assert item.element_kind == "record"
        suffix = _vector_list_suffix(item)
        write_lines.append(
            f"    write{suffix}List(writer, artifact.{spec.name.lower()});"
        )
    write_lines.append("}")
    write_lines.append("")

    for field in spec.fields:
        temps = _field_reader_temp(field)
        if temps:
            read_lines.append(f"    {temps}")
    if spec.item is not None:
        item = spec.item
        assert item.element_kind == "record"
        suffix = _vector_list_suffix(item)
        read_lines.append(
            f"    return read{suffix}List(reader, artifact.{spec.name.lower()}, "
            f"{item.max_items}) && reader.remaining() == 0;"
        )
    else:
        post = []
        for field in spec.fields:
            post.extend(_field_after_read(field, "artifact"))
        calls = []
        for field in spec.fields:
            if field.kind == "vector":
                suffix = _vector_list_suffix(field)
                calls.append(
                    f"read{suffix}List(reader, artifact.{field.name}, {field.max_items})"
                )
            else:
                calls.append(_field_read_call(field, "artifact"))
        if calls:
            read_lines.append("    const bool ok = " + " &&\n           ".join(calls) + ";")
            read_lines.append("    if (!ok)")
            read_lines.append("        return false;")
            read_lines.extend(post)
            read_lines.append("    return reader.remaining() == 0;")
        else:
            read_lines.append("    return reader.remaining() == 0;")
    read_lines.append("}")
    read_lines.append("")
    return write_lines + read_lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("rules", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    rules = parse_rules(args.rules.read_text(encoding="utf-8"), args.rules)
    files = [
        ("cache-section.hpp", "\n".join(header_lines(rules))),
        ("cache-section.cpp", "\n".join(source_lines(rules))),
        ("cache-codec.gen.hpp", "\n".join(codec_header_lines(rules))),
        ("cache-codec.gen.cpp", "\n".join(codec_source_lines(rules))),
        (
            ".gitignore",
            gitignore_lines(
                [
                    "cache-section.hpp",
                    "cache-section.cpp",
                    "cache-codec.gen.hpp",
                    "cache-codec.gen.cpp",
                ]
            ),
        ),
    ]
    write_generated_files(args.out, files)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
