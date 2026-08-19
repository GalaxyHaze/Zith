#!/usr/bin/env python3
"""Regression tests for the type-system generator."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.test_kit import (  # noqa: E402
    assert_contains,
    compile_smoke,
    expect_error,
    run_generator,
    write_rules,
)

VALID_RULES = """\
[metadata]
name = "toolkit-types"
version = 1

[types]
void    : primitive
bool    : primitive
char    : primitive
i1      : integer bits=1 signed
i8      : integer bits=8 signed
i16     : integer bits=16 signed
i32     : integer bits=32 signed
i64     : integer bits=64 signed
u8      : integer bits=8
u16     : integer bits=16
u32     : integer bits=32
u64     : integer bits=64
f32     : float bits=32
f64     : float bits=64
string  : opaque
ptr     : pointer
array   : array
slice   : slice
fn      : function
opt     : optional
struct  : nominal
enum    : nominal
union   : nominal
userdef : userdef

[coercions]
i8  -> i16 = implicit
i16 -> i32 = implicit
i32 -> i64 = implicit
i8  -> i32 = implicit
i8  -> i64 = implicit
i16 -> i64 = implicit
bool -> i8 = implicit
bool -> i32 = implicit
char -> i32 = implicit
char -> i64 = implicit
f32 -> f64 = implicit
i8  -> f32 = implicit
i16 -> f32 = implicit
i32 -> f64 = implicit
i64 -> f64 = explicit
i32 -> f32 = explicit

[casts]
i32 -> i8  = trunc
i64 -> i32 = trunc
i32 -> f32 = round
f64 -> i32 = trunc
u32 -> u64 = zero_extend

[common]
i8   , i16 = i16
i16  , i32 = i32
i32  , i64 = i64
i8   , i32 = i32
i8   , f32 = f32
i16  , f32 = f32
i32  , f64 = f64
bool , i64 = i64
char , i32 = i32
char , i64 = i64
f32  , f64 = f64
"""

SMOKE_CPP = """\
#include "type-system-table.hpp"

#include <string_view>

int main() {
    using namespace toolkit::type_system;
    const TypeDesc *i32 = staticFindType("i32");
    if (!i32 || i32->kind != TypeKind::I32 || i32->bits != 32)
        return 1;
    if (!i32->isSigned)
        return 2;
    if (staticFindType("void")->kind != TypeKind::Void)
        return 3;
    if (staticCoercionKind(i32->id, staticFindType("i64")->id) !=
        CoercionKind::Implicit)
        return 4;
    if (staticCastKind(staticFindType("i64")->id, i32->id) !=
        CastKind::Truncate)
        return 5;
    TypeId out = kInvalidTypeId;
    if (!staticCommonType(
            staticFindType("i8")->id, staticFindType("i16")->id, out) ||
        staticTypeById(out) != staticFindType("i16"))
        return 6;
    return 0;
}
"""


def main() -> int:
    repo_root = Path(sys.argv[1]).resolve()
    compiler = sys.argv[2]
    generator = repo_root / "src/common/type-system/generate.py"

    with tempfile.TemporaryDirectory(
        prefix="type-system-generator-", dir="/tmp"
    ) as tmp_dir_name:
        tmpdir = Path(tmp_dir_name)
        type_include = tmpdir / "include" / "common" / "type-system"
        type_include.mkdir(parents=True)

        rules = write_rules(tmpdir, VALID_RULES, "type-system")
        result = run_generator(repo_root, rules, type_include, generator=generator)
        if result.returncode != 0:
            raise AssertionError(result.stderr)

        header = (type_include / "type-system-table.hpp").read_text(
            encoding="utf-8"
        )
        assert_contains(header, "kTypeTable", "type table")
        assert_contains(header, "kCoercionTable", "coercion table")
        assert_contains(header, "kCastTable", "cast table")
        assert_contains(header, "kCommonTable", "common table")
        assert_contains(header, "TypeKind::I8", "integer type")
        assert_contains(header, "CoercionKind::Implicit", "implicit coercion")
        assert_contains(header, "CastKind::Truncate", "trunc cast")
        assert_contains(header, "staticCommonType", "common lookup")

        smoke = type_include / "smoke.cpp"
        smoke.write_text(SMOKE_CPP, encoding="utf-8")
        compile_smoke(
            repo_root,
            compiler,
            tmpdir / "include",
            source=smoke,
        )

        expect_error(
            repo_root,
            tmpdir,
            VALID_RULES + "\n[unknown]\nvalue: Bool\n",
            "seccao desconhecida",
            "unknown section",
            generator=generator,
            generator_kind="type-system",
        )
        expect_error(
            repo_root,
            tmpdir,
            VALID_RULES.replace(
                "i32     : integer bits=32 signed\n",
                "i32     : integer bits=32 signed\n"
                "i32     : integer bits=32 signed\n",
            ),
            "tipo duplicado",
            "duplicate type",
            generator=generator,
            generator_kind="type-system",
        )
        expect_error(
            repo_root,
            tmpdir,
            VALID_RULES.replace(
                "[coercions]\n",
                "[coercions]\nmissing -> i32 = implicit\n",
            ),
            "tipo nao declarado",
            "unknown coercion type",
            generator=generator,
            generator_kind="type-system",
        )
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
