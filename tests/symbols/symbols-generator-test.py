#!/usr/bin/env python3
"""Regression tests for the symbols generator."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path

REPO_ROOT = Path(sys.argv[1])
COMPILER  = sys.argv[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.test_kit.asserts import assert_contains, assert_not_contains
from tools.test_kit.smoke import compile_smoke, expect_error, run_generator, write_rules

_GENERATOR = REPO_ROOT / "src/symbols/generate.py"
_BUILD_SRC = REPO_ROOT / "build" / "src"
_INCLUDE_ROOT = _BUILD_SRC / "symbols"


def _gen(rules_text: str, out_dir: Path) -> subprocess.CompletedProcess[str]:
    rules_path = write_rules(out_dir, rules_text, "symbols")
    return run_generator(REPO_ROOT, rules_path, out_dir, generator=_GENERATOR)


def test_valid_rules() -> None:
    rules = textwrap.dedent("""\
        [kinds]
        Fn
        Struct

        [data]
        name: uint32_t
        kind: uint8_t
    """)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        result = _gen(rules, out)
        assert result.returncode == 0, f"generator failed: {result.stderr}"
        hpp = (out / "symbols.hpp").read_text()
        assert_contains(hpp, "enum class SymKind", "SymKind enum")
        assert_contains(hpp, "    Fn,", "Fn kind")
        assert_contains(hpp, "    Struct,", "Struct kind")
        assert_contains(hpp, "struct SymbolData {", "SymbolData struct")
        assert_contains(hpp, "    uint32_t name = 0;", "name field")
        assert_contains(hpp, "    uint8_t kind = 0;", "kind field")
        assert_contains(hpp, "inline const char *symKindName", "symKindName inline")
        assert_contains(hpp, "case SymKind::Fn:", "Fn case in header")
        assert_contains(hpp, "case SymKind::Struct:", "Struct case in header")
        assert_not_contains(hpp, "makeSymbol", "minimal rules have no makeSymbol")
        assert_not_contains(hpp, "visibilityKindName",
                            "minimal rules have no visibility helpers")
        assert_not_contains(
            hpp,
            "SymId() = default",
            "SymId must not retain a default constructor",
        )
        # symbols.cpp is no longer generated (header-only)
        cpp_path = out / "symbols.cpp"
        assert not cpp_path.exists(), "symbols.cpp should not be generated"
        print("  ok - valid rules produce expected output")


def test_canonical_rules_emit_helpers() -> None:
    rules = textwrap.dedent("""\
        [kinds]
        Fn
        Struct

        [data]
        name: InternedId
        scope: uint32_t
        visibility: SymbolVisibility
        mod_depth: int32_t
        kind: SymKind
        decl_id: uint32_t
        span: common::memory::Span
        doc_span: common::memory::Span
        target: SymId
        members: common::memory::DynArray<SymId>
    """)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        result = _gen(rules, out)
        assert result.returncode == 0, f"generator failed: {result.stderr}"
        hpp = (out / "symbols.hpp").read_text()
        assert_contains(hpp, "makeSymbol", "makeSymbol helper")
        assert_contains(hpp, "visibilityKindName", "visibilityKindName helper")
        assert_contains(hpp, "visibilityName", "visibilityName helper")
        assert_contains(hpp, "kInvalidSymId", "makeSymbol initializes target")
        assert_contains(hpp, "members", "makeSymbol initializes members")
        print("  ok - canonical rules emit symbol helpers")


def test_smoke_compile() -> None:
    rules = textwrap.dedent("""\
        [kinds]
        Fn
        Struct

        [data]
        name: uint32_t
        kind: uint8_t
    """)
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        _gen(rules, base)
        smoke = base / "smoke.cpp"
        smoke.write_text(textwrap.dedent("""\
            #include "symbols/symbols.hpp"

            #include <type_traits>

            static_assert(!std::is_default_constructible_v<toolkit::symbols::SymId>);

            int main() {
                const auto invalid = toolkit::symbols::kInvalidSymId;
                return invalid.module == toolkit::symbols::kInvalidModule &&
                               invalid.local == 0
                           ? 0
                           : 1;
            }
        """))
        compile_smoke(REPO_ROOT, COMPILER, base, source=smoke, include_dirs=[_BUILD_SRC])
        print("  ok - generated symbols header compiles")


def test_duplicate_kind() -> None:
    expect_error(
        REPO_ROOT,
        Path(tempfile.mkdtemp()),
        textwrap.dedent("""\
            [kinds]
            Fn
            Fn

            [data]
            name: uint32_t
        """),
        "kind repetido",
        "duplicate kind",
        generator=_GENERATOR,
        generator_kind="symbols",
    )
    print("  ok - duplicate kind is rejected")


def test_duplicate_field() -> None:
    expect_error(
        REPO_ROOT,
        Path(tempfile.mkdtemp()),
        textwrap.dedent("""\
            [kinds]
            Fn

            [data]
            name: uint32_t
            name: uint32_t
        """),
        "campo repetido",
        "duplicate field",
        generator=_GENERATOR,
        generator_kind="symbols",
    )
    print("  ok - duplicate field is rejected")


def test_invalid_kind_name() -> None:
    expect_error(
        REPO_ROOT,
        Path(tempfile.mkdtemp()),
        textwrap.dedent("""\
            [kinds]
            1Fn

            [data]
            name: uint32_t
        """),
        "nome de kind invalido",
        "invalid kind name",
        generator=_GENERATOR,
        generator_kind="symbols",
    )
    print("  ok - invalid kind name is rejected")


def test_invalid_field_name() -> None:
    expect_error(
        REPO_ROOT,
        Path(tempfile.mkdtemp()),
        textwrap.dedent("""\
            [kinds]
            Fn

            [data]
            Name: uint32_t
        """),
        "nome de campo invalido",
        "invalid field name (capitalized)",
        generator=_GENERATOR,
        generator_kind="symbols",
    )
    print("  ok - capitalized field name is rejected")


def test_unknown_section() -> None:
    expect_error(
        REPO_ROOT,
        Path(tempfile.mkdtemp()),
        textwrap.dedent("""\
            [kinds]
            Fn

            [unknown]
            x: int
        """),
        "seccao desconhecida",
        "unknown section",
        generator=_GENERATOR,
        generator_kind="symbols",
    )
    print("  ok - unknown section is rejected")


def test_empty_file() -> None:
    expect_error(
        REPO_ROOT,
        Path(tempfile.mkdtemp()),
        "",
        "[kinds] tem de declarar pelo menos um kind",
        "empty file",
        generator=_GENERATOR,
        generator_kind="symbols",
    )
    print("  ok - empty file is rejected")


def test_missing_data() -> None:
    expect_error(
        REPO_ROOT,
        Path(tempfile.mkdtemp()),
        textwrap.dedent("""\
            [kinds]
            Fn
        """),
        "[data] tem de declarar",
        "missing data section",
        generator=_GENERATOR,
        generator_kind="symbols",
    )
    print("  ok - missing [data] section is rejected")


if __name__ == "__main__":
    print("symbols generator regression tests")
    test_valid_rules()
    test_canonical_rules_emit_helpers()
    test_smoke_compile()
    test_duplicate_kind()
    test_duplicate_field()
    test_invalid_kind_name()
    test_invalid_field_name()
    test_unknown_section()
    test_empty_file()
    test_missing_data()
    print("PASS")
