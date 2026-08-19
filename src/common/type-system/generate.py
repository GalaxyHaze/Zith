#!/usr/bin/env python3
"""Generate type-system-table.hpp and .gitignore from type-system.rules."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
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
_SECTIONS = {"metadata", "types", "coercions", "casts", "common"}
_CATEGORIES = {
    "primitive": "Primitive",
    "integer": "Integer",
    "float": "Float",
    "opaque": "Opaque",
    "pointer": "Pointer",
    "array": "Array",
    "slice": "Slice",
    "function": "Function",
    "optional": "Optional",
    "nominal": "Nominal",
    "userdef": "UserDefined",
}
_CAST_KINDS = {
    "trunc": "Truncate",
    "sign_extend": "SignExtend",
    "zero_extend": "ZeroExtend",
    "round": "Round",
    "reinterpret": "Reinterpret",
    "bitcast": "Bitcast",
    "ignored_maybe": "IgnoredMaybe",
}
_CPP_NAMES = {
    "void": "Void",
    "bool": "Bool",
    "char": "Char",
    "i1": "I1",
    "i8": "I8",
    "i16": "I16",
    "i32": "I32",
    "i64": "I64",
    "u8": "U8",
    "u16": "U16",
    "u32": "U32",
    "u64": "U64",
    "f32": "F32",
    "f64": "F64",
    "string": "String",
    "ptr": "Pointer",
    "array": "Array",
    "slice": "Slice",
    "fn": "Function",
    "opt": "Optional",
    "struct": "Struct",
    "enum": "Enum",
    "union": "Union",
    "userdef": "UserDefined",
}
_BITS_REQUIRED = {"integer", "float"}


@dataclass(frozen=True)
class TypeEntry:
    name: str
    category: str
    bits: int
    signed: bool


@dataclass(frozen=True)
class CoercionRow:
    left: str
    right: str
    kind: str


@dataclass(frozen=True)
class CastRow:
    left: str
    right: str
    kind: str


@dataclass(frozen=True)
class CommonRow:
    left: str
    right: str
    result: str


@dataclass(frozen=True)
class ParsedRules:
    types: tuple[TypeEntry, ...]
    coercions: tuple[CoercionRow, ...]
    casts: tuple[CastRow, ...]
    commons: tuple[CommonRow, ...]


def parse_rules(text: str, path: Path) -> ParsedRules:
    metadata_name = ""
    metadata_version = -1
    types: list[TypeEntry] = []
    type_names: dict[str, TypeEntry] = {}
    coercions: list[CoercionRow] = []
    casts: list[CastRow] = []
    commons: list[CommonRow] = []
    section = ""
    section_seen: set[str] = set()

    error_prefix = path.name if path.name else "type-system.rules"

    def require_type(name: str, line_no: int, table: str) -> str:
        if name not in type_names:
            raise RuleError(
                line_no, f"tipo nao declarado em [{table}]: {name!r}"
            )
        return name

    for line_no, line in join_logical_lines(text):
        header = _HEADER_RE.fullmatch(line)
        if header:
            raw_section = header.group(1).strip()
            section = raw_section.lower()
            if section not in _SECTIONS:
                raise RuleError(line_no, f"seccao desconhecida: [{raw_section}]")
            if section in section_seen:
                raise RuleError(line_no, f"seccao repetida: [{raw_section}]")
            section_seen.add(section)
            continue

        if section == "metadata":
            key, sep, value = line.partition("=")
            key = key.strip()
            value = value.strip()
            if not sep or not key or not value:
                raise RuleError(line_no, f"campo de metadata invalido: {line!r}")
            if key == "name":
                unquoted = value[1:-1] if (
                    len(value) >= 2 and value[0] == '"' and value[-1] == '"'
                ) else value
                if unquoted != "toolkit-types":
                    raise RuleError(
                        line_no, f"metadata name invalido: {value!r}"
                    )
                metadata_name = unquoted
                continue
            if key == "version":
                try:
                    metadata_version = int(value)
                except ValueError as exc:
                    raise RuleError(
                        line_no, f"metadata version invalido: {value!r}"
                    ) from exc
                if metadata_version != 1:
                    raise RuleError(
                        line_no, f"metadata version invalido: {value!r}"
                    )
                continue
            raise RuleError(line_no, f"campo de metadata desconhecido: {key!r}")

        if section == "types":
            name, sep, rest = line.partition(":")
            name = name.strip()
            rest = rest.strip()
            if not sep or not name or not rest:
                raise RuleError(line_no, f"entrada de tipo invalida: {line!r}")
            validate_identifier(name, line_no, "tipo")
            category_raw, _, attrs_text = rest.partition(" ")
            category = _CATEGORIES.get(category_raw)
            if category is None:
                raise RuleError(
                    line_no,
                    f"categoria invalida para tipo {name!r}: {category_raw!r}",
                )
            attrs = [token for token in attrs_text.split() if token]
            bits = 0
            signed = False
            for token in attrs:
                if token == "signed":
                    signed = True
                    continue
                if token == "unsigned":
                    signed = False
                    continue
                bits_match = re.fullmatch(r"bits=(\d+)", token)
                if bits_match:
                    bits = int(bits_match.group(1))
                    if bits <= 0:
                        raise RuleError(
                            line_no, f"bits invalido para tipo {name!r}: {bits}"
                        )
                    continue
                raise RuleError(line_no, f"atributo de tipo desconhecido: {token!r}")
            if category_raw in _BITS_REQUIRED and bits <= 0:
                raise RuleError(
                    line_no, f"{category_raw} {name!r} requer bits>0"
                )
            if category_raw not in _BITS_REQUIRED and attrs:
                raise RuleError(
                    line_no,
                    "atributos bits/signed permitidos apenas para integer/float: "
                    f"{line!r}",
                )
            if name in type_names:
                raise RuleError(line_no, f"tipo duplicado: {name!r}")
            entry = TypeEntry(
                name=name,
                category=category,
                bits=bits,
                signed=signed,
            )
            type_names[name] = entry
            types.append(entry)
            continue

        if section == "coercions":
            left, sep, rest = line.partition("->")
            if not sep:
                raise RuleError(line_no, f"entrada de coercao invalida: {line!r}")
            right, sep2, kind = rest.partition("=")
            left = left.strip()
            right = right.strip()
            kind = kind.strip()
            if not sep2 or kind not in {"implicit", "explicit"}:
                raise RuleError(
                    line_no,
                    "kind de coercao invalido "
                    "(esperado implicit|explicit): "
                    f"{line!r}",
                )
            require_type(left, line_no, "coercions")
            require_type(right, line_no, "coercions")
            if left == right:
                raise RuleError(
                    line_no, f"coercao de tipo para si proprio: {left!r}"
                )
            coercions.append(CoercionRow(left=left, right=right, kind=kind))
            continue

        if section == "casts":
            left, sep, rest = line.partition("->")
            if not sep:
                raise RuleError(line_no, f"entrada de cast invalida: {line!r}")
            right, sep2, kind = rest.partition("=")
            left = left.strip()
            right = right.strip()
            kind = kind.strip()
            if not sep2 or kind not in _CAST_KINDS:
                raise RuleError(
                    line_no,
                    "kind de cast invalido (esperado trunc|sign_extend|"
                    "zero_extend|round|reinterpret|bitcast|ignored_maybe): "
                    f"{line!r}",
                )
            require_type(left, line_no, "casts")
            require_type(right, line_no, "casts")
            if left == right:
                raise RuleError(line_no, f"cast de tipo para si proprio: {left!r}")
            casts.append(CastRow(left=left, right=right, kind=kind))
            continue

        if section == "common":
            operands, sep, result = line.partition("=")
            left_raw, comma, right_raw = operands.partition(",")
            left = left_raw.strip()
            right = right_raw.strip()
            result = result.strip()
            if not sep or not comma or not left or not right or not result:
                raise RuleError(line_no, f"entrada de common invalida: {line!r}")
            require_type(left, line_no, "common")
            require_type(right, line_no, "common")
            require_type(result, line_no, "common")
            commons.append(CommonRow(left=left, right=right, result=result))
            continue

        raise RuleError(line_no, f"campo fora de secao: {line!r}")

    if metadata_name != "toolkit-types":
        raise RuleError(1, "metadata name tem de ser toolkit-types")
    if metadata_version != 1:
        raise RuleError(1, "metadata version tem de ser 1")
    if not types:
        raise RuleError(1, f"{error_prefix} tem de declarar pelo menos um tipo")
    if not coercions:
        raise RuleError(1, "[coercions] tem de declarar pelo menos uma coercao")
    if not casts:
        raise RuleError(1, "[casts] tem de declarar pelo menos um cast")
    if not commons:
        raise RuleError(1, "[common] tem de declarar pelo menos um common")

    coercion_keys: set[tuple[str, str]] = set()
    for row in coercions:
        key = (row.left, row.right)
        if key in coercion_keys:
            raise RuleError(len(types) + 1, f"coercao duplicada: {key!r}")
        coercion_keys.add(key)

    cast_keys: set[tuple[str, str]] = set()
    for row in casts:
        key = (row.left, row.right)
        if key in cast_keys:
            raise RuleError(len(types) + 1, f"cast duplicado: {key!r}")
        cast_keys.add(key)

    common_keys: set[tuple[str, str]] = set()
    for row in commons:
        key = (row.left, row.right)
        symmetric = (row.right, row.left)
        if key in common_keys or symmetric in common_keys:
            raise RuleError(len(types) + 1, f"common duplicado: {key!r}")
        common_keys.add(key)

    return ParsedRules(
        types=tuple(types),
        coercions=tuple(coercions),
        casts=tuple(casts),
        commons=tuple(commons),
    )


def _type_index(types: tuple[TypeEntry, ...], name: str) -> int:
    for index, entry in enumerate(types):
        if entry.name == name:
            return index
    raise AssertionError(f"unresolved type {name!r}")


def _cpp_name(name: str) -> str:
    return _CPP_NAMES[name]


def _make_header(rules: ParsedRules) -> str:
    out: list[str] = []
    out.append(
        "// auto-generated by src/common/type-system/generate.py - do not edit by hand"
    )
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstddef>")
    out.append("#include <cstdint>")
    out.append("#include <string_view>")
    out.append("")
    out.append("namespace toolkit::type_system {")
    out.append("")
    out.append("using TypeId = std::uint32_t;")
    out.append("inline constexpr TypeId kInvalidTypeId = ~TypeId{0};")
    out.append("")

    out.append("class TypeContext;")
    out.append("")
    out.append("enum class TypeKind : std::uint8_t {")
    for index, entry in enumerate(rules.types):
        comma = "," if index + 1 < len(rules.types) else ""
        out.append(f"    {_cpp_name(entry.name)}{comma}")
    out.append("};")
    out.append("")

    categories = ["Primitive", "Integer", "Float", "Opaque", "Pointer",
                  "Array", "Slice", "Function", "Optional", "Nominal",
                  "UserDefined"]
    out.append("enum class Category : std::uint8_t {")
    for index, category in enumerate(categories):
        comma = "," if index + 1 < len(categories) else ""
        out.append(f"    {category}{comma}")
    out.append("};")
    out.append("")

    out.append(
        "enum class CoercionKind : std::uint8_t { None, Implicit, Explicit };"
    )
    out.append("enum class CastKind : std::uint8_t {")
    out.append("    None,")
    out.append("    Truncate,")
    out.append("    SignExtend,")
    out.append("    ZeroExtend,")
    out.append("    Reinterpret,")
    out.append("    Round,")
    out.append("    Bitcast,")
    out.append("    IgnoredMaybe,")
    out.append("};")
    out.append("")

    out.append("struct TypeDesc {")
    out.append("    std::string_view name;")
    out.append("    TypeId id = kInvalidTypeId;")
    out.append("    TypeKind kind = TypeKind::Void;")
    out.append("    Category category = Category::Primitive;")
    out.append("    std::uint16_t bits = 0;")
    out.append("    bool isSigned = false;")
    out.append("    const TypeId *components = nullptr;")
    out.append("    std::size_t componentCount = 0;")
    out.append("};")
    out.append("")

    out.append("struct CoercionRule {")
    out.append("    TypeId from = kInvalidTypeId;")
    out.append("    TypeId to = kInvalidTypeId;")
    out.append("    CoercionKind kind = CoercionKind::None;")
    out.append("};")
    out.append("")
    out.append("struct CastRule {")
    out.append("    TypeId from = kInvalidTypeId;")
    out.append("    TypeId to = kInvalidTypeId;")
    out.append("    CastKind kind = CastKind::None;")
    out.append("};")
    out.append("")
    out.append("struct CommonRule {")
    out.append("    TypeId left = kInvalidTypeId;")
    out.append("    TypeId right = kInvalidTypeId;")
    out.append("    TypeId result = kInvalidTypeId;")
    out.append("};")
    out.append("")

    out.append("using UnaryResolver = ")
    out.append("    TypeId (*)(void *, const TypeContext &, TypeId);")
    out.append("using BinaryResolver = TypeId (*)(void *, const TypeContext &, "
               "TypeId, TypeId);")
    out.append("")
    out.append("struct UnaryRule {")
    out.append("    std::string_view op;")
    out.append("    TypeId operand = kInvalidTypeId;")
    out.append("    TypeId result = kInvalidTypeId;")
    out.append("    UnaryResolver resolver = nullptr;")
    out.append("    void *userData = nullptr;")
    out.append("};")
    out.append("")
    out.append("struct BinaryRule {")
    out.append("    std::string_view op;")
    out.append("    TypeId left = kInvalidTypeId;")
    out.append("    TypeId right = kInvalidTypeId;")
    out.append("    TypeId result = kInvalidTypeId;")
    out.append("    BinaryResolver resolver = nullptr;")
    out.append("    void *userData = nullptr;")
    out.append("};")
    out.append("")

    out.append(f"inline constexpr std::size_t kTypeTableCount = {len(rules.types)};")
    out.append("inline constexpr TypeDesc kTypeTable[] = {")
    for index, entry in enumerate(rules.types):
        out.append("    {")
        out.append(f"        .name = {json.dumps(entry.name)},")
        out.append(f"        .id = {index},")
        out.append(f"        .kind = TypeKind::{_cpp_name(entry.name)},")
        out.append(f"        .category = Category::{entry.category},")
        out.append(f"        .bits = {entry.bits},")
        out.append(f"        .isSigned = {'true' if entry.signed else 'false'},")
        out.append("    },")
    out.append("};")
    out.append("")

    out.append(
        f"inline constexpr std::size_t kCoercionTableCount = {len(rules.coercions)};"
    )
    out.append("inline constexpr CoercionRule kCoercionTable[] = {")
    for row in rules.coercions:
        kind = "Implicit" if row.kind == "implicit" else "Explicit"
        out.append("    {")
        out.append(
            f"        .from = TypeId({_type_index(rules.types, row.left)}),"
        )
        out.append(
            f"        .to = TypeId({_type_index(rules.types, row.right)}),"
        )
        out.append(f"        .kind = CoercionKind::{kind},")
        out.append("    },")
    out.append("};")
    out.append("")

    out.append(f"inline constexpr std::size_t kCastTableCount = {len(rules.casts)};")
    out.append("inline constexpr CastRule kCastTable[] = {")
    for row in rules.casts:
        out.append("    {")
        out.append(f"        .from = TypeId({_type_index(rules.types, row.left)}),")
        out.append(f"        .to = TypeId({_type_index(rules.types, row.right)}),")
        out.append(f"        .kind = CastKind::{_CAST_KINDS[row.kind]},")
        out.append("    },")
    out.append("};")
    out.append("")

    out.append(f"inline constexpr std::size_t kCommonTableCount = {len(rules.commons)};")
    out.append("inline constexpr CommonRule kCommonTable[] = {")
    for row in rules.commons:
        out.append("    {")
        out.append(f"        .left = TypeId({_type_index(rules.types, row.left)}),")
        out.append(f"        .right = TypeId({_type_index(rules.types, row.right)}),")
        out.append(f"        .result = TypeId({_type_index(rules.types, row.result)}),")
        out.append("    },")
    out.append("};")
    out.append("")

    out.append(
        "[[nodiscard]] inline const TypeDesc *"
        "staticFindType(std::string_view name) noexcept {"
    )
    out.append("    for (const TypeDesc &entry : kTypeTable) {")
    out.append("        if (entry.name == name)")
    out.append("            return &entry;")
    out.append("    }")
    out.append("    return nullptr;")
    out.append("}")
    out.append("")

    out.append(
        "[[nodiscard]] inline const TypeDesc *"
        "staticTypeById(TypeId id) noexcept {"
    )
    out.append("    return id < kTypeTableCount ? &kTypeTable[id] : nullptr;")
    out.append("}")
    out.append("")

    out.append(
        "[[nodiscard]] inline CoercionKind "
        "staticCoercionKind(TypeId from, TypeId to) noexcept {"
    )
    out.append("    for (const CoercionRule &row : kCoercionTable) {")
    out.append("        if (row.from == from && row.to == to)")
    out.append("            return row.kind;")
    out.append("    }")
    out.append("    return CoercionKind::None;")
    out.append("}")
    out.append("")

    out.append(
        "[[nodiscard]] inline CastKind "
        "staticCastKind(TypeId from, TypeId to) noexcept {"
    )
    out.append("    for (const CastRule &row : kCastTable) {")
    out.append("        if (row.from == from && row.to == to)")
    out.append("            return row.kind;")
    out.append("    }")
    out.append("    return CastKind::None;")
    out.append("}")
    out.append("")

    out.append(
        "[[nodiscard]] inline bool "
        "staticCommonType(TypeId left, TypeId right, TypeId &out) noexcept {"
    )
    out.append("    for (const CommonRule &row : kCommonTable) {")
    out.append("        if ((row.left == left && row.right == right) ||")
    out.append("            (row.left == right && row.right == left)) {")
    out.append("            out = row.result;")
    out.append("            return true;")
    out.append("        }")
    out.append("    }")
    out.append("    return false;")
    out.append("}")
    out.append("")

    out.append("} // namespace toolkit::type_system")
    out.append("")
    return "\n".join(out)


def _make_gitignore() -> str:
    return gitignore_lines(["type-system-table.hpp"])


def write_generated(out_dir: Path, rules: ParsedRules) -> list[Path]:
    return write_generated_files(
        out_dir,
        [
            ("type-system-table.hpp", _make_header(rules)),
            (".gitignore", _make_gitignore()),
        ],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "rules",
        nargs="?",
        default="src/common/type-system/type-system.rules",
    )
    parser.add_argument(
        "--out",
        default="build/src/common/type-system",
        help="diretorio de saida",
    )
    args = parser.parse_args()

    rules_path = Path(args.rules)
    out_path = Path(args.out)
    if not rules_path.exists():
        print(f"rules file not found: {rules_path}", file=sys.stderr)
        return 2

    try:
        rules = parse_rules(rules_path.read_text(encoding="utf-8"), rules_path)
    except RuleError as exc:
        print(exc.render(rules_path), file=sys.stderr)
        return 2

    written = write_generated(out_path, rules)
    for target in written:
        print(f"generated {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
