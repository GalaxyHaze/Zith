#!/usr/bin/env python3

from __future__ import annotations

import sys
import tempfile
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.test_kit import (
    assert_contains,
    assert_not_contains,
    compile_smoke,
    run_generator,
    write_rules,
)

VALID_RULES = """\
[format]
name = "smoke-cache"
magic = "smoke"
version = 3
endian = "little"

[metadata]
cacheKeyHash: std::uint32_t

[sections]
Metadata
Deps

[section.Metadata]
fields: []

[section.Deps]
items: vector[DependencyRecord] max_items = 4096

[record.DependencyRecord]
cpp = "toolkit::cache::DependencyRecord"
canonical_path: string
import_key: string
public_abi_hi: u32
public_abi_lo: u32

[encoding]
length: uint32
align: 8
checksum: fnv1a64
"""

SMOKE_CPP = """\
#include "cache/cache-section.hpp"

#include <cstdint>

int main() {
    static_assert(toolkit::cache::kCacheHeaderSize >= 64, "expected fixed header");
    toolkit::cache::CacheFile file{};
    return file.section(toolkit::cache::SectionId::Deps).empty() ? 0 : 1;
}
"""


def main() -> int:
    repo_root = Path(sys.argv[1]).resolve()
    compiler = sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="cache-generator-", dir="/tmp") as tmp:
        tmpdir = Path(tmp)
        include_root = tmpdir / "include"
        out_dir = include_root / "cache"
        out_dir.mkdir(parents=True)
        rules = write_rules(tmpdir, VALID_RULES, "cache")
        result = run_generator(
            repo_root,
            rules,
            out_dir,
            generator=repo_root / "src/cache/generate.py",
        )
        if result.returncode != 0:
            raise AssertionError(result.stderr)
        header = (out_dir / "cache-section.hpp").read_text(encoding="utf-8")
        source = (out_dir / "cache-section.cpp").read_text(encoding="utf-8")
        assert_contains(header, "enum class SectionId : std::uint8_t", "section enum")
        assert_contains(header, "struct SectionEntry", "section entry")
        assert_contains(header, "kCacheHeaderSize", "header size constant")
        assert_contains(source, "CacheFile::section", "section accessor")
        assert_contains(source, "CacheFile::valid", "validity check")
        assert_not_contains(header, "Metadata2", "no unexpected section")
        codec_header = (out_dir / "cache-codec.gen.hpp").read_text(encoding="utf-8")
        codec_source = (out_dir / "cache-codec.gen.cpp").read_text(encoding="utf-8")
        assert_contains(codec_header, "void serializeDependency(", "record serialization")
        assert_contains(codec_source, "bool readDeps(", "section reader")
        (include_root / "smoke.cpp").write_text(SMOKE_CPP, encoding="utf-8")
        compile_smoke(repo_root, compiler, include_root)

        missing_format_text = VALID_RULES.replace(
            '[format]\nname = "smoke-cache"\nmagic = "smoke"\nversion = 3\nendian = "little"\n\n',
            "",
        )
        missing_format = run_generator(
            repo_root,
            write_rules(tmpdir, missing_format_text, "cache"),
            out_dir,
            generator=repo_root / "src/cache/generate.py",
        )
        if missing_format.returncode == 0 or "[format] tem de declarar" not in missing_format.stderr:
            raise AssertionError("generator accepted rules without [format]")

        duplicate_section = run_generator(
            repo_root,
            write_rules(
                tmpdir,
                VALID_RULES + "\n[metadata]\nother: uint32\n[sections]\nDeps\nDeps\n",
                "cache",
            ),
            out_dir,
            generator=repo_root / "src/cache/generate.py",
        )
        if duplicate_section.returncode == 0 or "seccao repetida" not in duplicate_section.stderr:
            raise AssertionError("generator accepted duplicate sections")

        missing_record = run_generator(
            repo_root,
            write_rules(
                tmpdir,
                VALID_RULES.replace(
                    "[section.Deps]\nitems: vector[DependencyRecord] max_items = 4096\n\n",
                    "[section.Deps]\nitems: vector[MissingRecord] max_items = 4096\n\n",
                ),
                "cache",
            ),
            out_dir,
            generator=repo_root / "src/cache/generate.py",
        )
        if missing_record.returncode == 0 or "tipo desconhecido" not in missing_record.stderr:
            raise AssertionError("generator accepted an unknown record reference")

        bad_enum_value = run_generator(
            repo_root,
            write_rules(
                tmpdir,
                VALID_RULES
                + "\n[enum.Smoke]\ncpp = \"toolkit::cache::Smoke\"\nA = 0\nB = 0\n",
                "cache",
            ),
            out_dir,
            generator=repo_root / "src/cache/generate.py",
        )
        if bad_enum_value.returncode == 0 or "valor de enum duplicado" not in bad_enum_value.stderr:
            raise AssertionError("generator accepted duplicate enum values")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
