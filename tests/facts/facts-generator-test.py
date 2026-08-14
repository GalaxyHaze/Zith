#!/usr/bin/env python3
"""Regression tests for the facts generator."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.test_kit import (
    assert_contains,
    compile_smoke,
    expect_error,
    run_generator,
    write_rules,
)

VALID_RULES = """\
[enum.State]
Active = 0
Complete = 1
Failed = 2

[domain] Version
  attribute stable : Bool
  attribute major  : Int
  attribute state  : enum[State]

[domain] ResourceState
  attribute alive : Bool
  attribute dead  : Bool
  attribute lent  : Bool
"""

SMOKE_CPP = """\
#include "facts/facts.hpp"

#include <type_traits>

int main() {
    using toolkit::facts::FactStore;
    using toolkit::facts::ValueDomain;
    static_assert(std::is_same_v<toolkit::facts::VersionBoolStore, FactStore<bool>>);
    static_assert(std::is_same_v<toolkit::facts::ResourceStateLentStore, FactStore<bool>>);
    static_assert(std::is_same_v<toolkit::facts::VersionIntStore, FactStore<int>>);
    static_assert(std::is_same_v<toolkit::facts::VersionStateStore, FactStore<int>>);
    constexpr auto domain = ValueDomain<int>::exactValue(5);
    constexpr auto same = domain.intersect(domain);
    constexpr auto state = toolkit::facts::State::Complete;
    return same.kind == toolkit::facts::DomainKind::Exact &&
                   same.exact == 5 &&
                   toolkit::facts::kVersionStableFact.value == 0 &&
                   toolkit::facts::kVersionStateFact.value == 2 &&
                   toolkit::facts::kGeneratedFactIdCount == 6 &&
                   static_cast<int>(toolkit::facts::VersionAttribute::major) == 1 &&
                   state == toolkit::facts::State::Complete &&
                   toolkit::facts::kStateValueCount == 3
               ? 0
               : 1;
}
"""


def main() -> int:
    repo_root = Path(sys.argv[1]).resolve()
    compiler = sys.argv[2]

    with tempfile.TemporaryDirectory(prefix="facts-generator-", dir="/tmp") as tmp:
        tmpdir = Path(tmp)
        include_root = tmpdir / "include"
        out_dir = include_root / "facts"
        out_dir.mkdir(parents=True)

        rules = write_rules(tmpdir, VALID_RULES, "facts")
        result = run_generator(
            repo_root,
            rules,
            out_dir,
            generator=repo_root / "src/facts/generate.py",
        )
        if result.returncode != 0:
            raise AssertionError(result.stderr)

        header = (out_dir / "facts.hpp").read_text(encoding="utf-8")
        assert_contains(header, "enum class VersionAttribute", "Version enum")
        assert_contains(header, "using VersionStableStore = FactStore<bool>;", "Bool accessor")
        assert_contains(header, "using VersionMajorStore = FactStore<int>;", "Int accessor")
        assert_contains(header, "using VersionStateStore = FactStore<int>;", "enum accessor")
        assert_contains(header, "enum class State : std::int32_t {", "enum declaration")
        assert_contains(header, "Active = 0", "enum member")
        assert_contains(header, "Complete = 1", "enum member")
        assert_contains(header, "Failed = 2", "enum member")
        assert_contains(header, "State::Active", "enum value table")
        assert_contains(header, "State::Complete", "enum value table")
        assert_contains(header, "State::Failed", "enum value table")
        assert_contains(header, "inline constexpr std::uint16_t kStateValueCount =", "enum count")
        assert_contains(header, "inline constexpr State kStateValues[] = {", "enum values")
        assert_contains(header, "ResourceStateAttribute::lent", "ResourceState attribute")
        assert_contains(header, "kVersionStableFact", "stable fact id")
        assert_contains(header, "kVersionStateFact", "state fact id")
        assert_contains(header, "kResourceStateLentFact", "lent fact id")

        smoke = include_root / "smoke.cpp"
        smoke.write_text(SMOKE_CPP, encoding="utf-8")
        compile_smoke(repo_root, compiler, include_root, source=smoke)

        expect_error(
            repo_root,
            tmpdir,
            "[unknown]\nvalue: Bool\n",
            "seccao desconhecida",
            "unknown section",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
        expect_error(
            repo_root,
            tmpdir,
            "[domain] Version\nattribute stable : Bool\n[domain] Version\n",
            "dominio repetido",
            "duplicate domain",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
        expect_error(
            repo_root,
            tmpdir,
            "[domain] Version\nattribute stable : Bool\nattribute stable : Int\n",
            "atributo repetido",
            "duplicate attribute",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
        expect_error(
            repo_root,
            tmpdir,
            "[domain] Version\nattribute stable : String\n",
            "tipo invalido",
            "invalid attribute type",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
        expect_error(
            repo_root,
            tmpdir,
            "[enum.State]\nActive = 0\n[enum.State]\nActive = 1\n\
[domain] Version\nattribute state : enum[State]\n",
            "enum repetido",
            "duplicate enum",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
        expect_error(
            repo_root,
            tmpdir,
            "[enum.State]\nActive = 0\nActive = 1\n\
[domain] Version\nattribute state : enum[State]\n",
            "membro de enum duplicado",
            "duplicate enum member",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
        expect_error(
            repo_root,
            tmpdir,
            "[enum.State]\nActive = 0\nComplete = 0\n\
[domain] Version\nattribute state : enum[State]\n",
            "valor de enum duplicado",
            "duplicate enum value",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
        expect_error(
            repo_root,
            tmpdir,
            "[domain] Version\nattribute state : enum[Missing]\n",
            "enum desconhecido",
            "unknown enum type",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
        expect_error(
            repo_root,
            tmpdir,
            "[enum.State]\nActive = 2147483648\n\
[domain] Version\nattribute state : enum[State]\n",
            "valor de enum tem de caber em std::int32_t",
            "out-of-range enum value",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
        expect_error(
            repo_root,
            tmpdir,
            "[enum.State]\n[domain] Version\nattribute state : enum[State]\n",
            "tem de declarar pelo menos um membro",
            "empty enum",
            generator=repo_root / "src/facts/generate.py",
            generator_kind="facts",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
