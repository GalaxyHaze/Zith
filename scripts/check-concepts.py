#!/usr/bin/env python3
"""Check the Zith concept registry against the spec appendix.

The Zith surface is enumerated flat in the spec appendix (`Zith-spec.md`,
tables A.1 keywords, A.2 intrinsics, A.3 attributes). The concept registry in
`docs/concepts/registry.md` consolidates those surface forms per concept and
names the one document that owns each concept's meaning.

This check keeps the two from drifting. It fails when:

1. a surface form in the appendix table is missing from the registry;
2. a spelling in the registry is not in the appendix table;
3. an Authority citation (`path#anchor`) does not resolve to a real file and
   heading.

It is offline: it reads only files in the tree, so it can run in CI next to
`check-docs-consistency.py`. It also prints the count of `appendix` rows, which
is the consolidation gap: concepts that only the appendix table defines.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SPEC = "docs/Zith-spec.md"
REGISTRY = "docs/concepts/registry.md"

# Appendix subsections: (heading, label). A.1 and A.3 are next slices; this
# prototype wires A.2 through so the pipeline is proven end to end.
APPENDIX_TABLES = {
    "A.2 Compiler Intrinsics": "intrinsics",
}


class Inconsistency(ValueError):
    """Raised when the registry and the appendix disagree."""


def section(lines: list[str], heading: str) -> list[str]:
    """Return the lines under a heading, up to the next heading of any level."""
    start = None
    for index, line in enumerate(lines):
        if line.strip() == f"### {heading}":
            start = index + 1
            break
    if start is None:
        raise Inconsistency(f"missing section '### {heading}' in {SPEC}")
    end = len(lines)
    for index in range(start, len(lines)):
        if lines[index].startswith("#"):
            end = index
            break
    return lines[start:end]


def tables(block: list[str]) -> list[list[list[str]]]:
    """Return every pipe table in a block as a list of rows of cells."""
    found: list[list[list[str]]] = []
    current: list[list[str]] = []
    for line in block:
        stripped = line.strip()
        if stripped.startswith("|") and stripped.endswith("|"):
            current.append([cell.strip() for cell in stripped.strip("|").split("|")])
            continue
        if current:
            found.append(current)
            current = []
    if current:
        found.append(current)
    return found


def data_rows(table: list[list[str]]) -> list[list[str]]:
    """Drop the header and separator rows from a parsed table."""
    rows = []
    for row in table:
        if all(set(cell) <= {"-", ":", " "} for cell in row):
            continue
        rows.append(row)
    return rows[1:] if rows else []


def backticked(text: str) -> list[str]:
    """Return the contents of every backticked span, in order."""
    return re.findall(r"`([^`]+)`", text)


def surface_names(cell: str) -> list[str]:
    """Intrinsic names in one appendix cell, dropping parameters.

    `` `@sizeOf T` `` yields ``@sizeOf``; a cell may hold several spellings
    separated by `` / `` (`` `@file` / `@line` ``), each its own backtick span.
    """
    names = []
    for span in backticked(cell):
        token = span.split()[0].strip()
        if token.startswith("@"):
            names.append(token)
    return names


def appendix_surfaces(spec_lines: list[str], heading: str) -> set[str]:
    table = tables(section(spec_lines, heading))[0]
    surfaces: set[str] = set()
    for row in data_rows(table):
        surfaces.update(surface_names(row[0]))
    if not surfaces:
        raise Inconsistency(f"no surface forms parsed from '### {heading}'")
    return surfaces


def registry_rows(registry_lines: list[str]) -> list[list[str]]:
    rows: list[list[str]] = []
    for table in tables(registry_lines):
        header = [cell.replace("*", "").strip().lower() for cell in table[0]]
        if "concept" not in header or "authority" not in header:
            continue
        rows.extend(data_rows(table))
    if not rows:
        raise Inconsistency(f"no concept table found in {REGISTRY}")
    return rows


def registry_surfaces(rows: list[list[str]]) -> set[str]:
    surfaces: set[str] = set()
    for row in rows:
        surfaces.update(backticked(row[1]))
    return surfaces


def slugify(heading: str) -> str:
    """Approximate the GitHub heading slug for anchor resolution."""
    text = heading.lstrip("#").strip().lower()
    text = re.sub(r"[^\w\s-]", "", text)
    text = re.sub(r"\s+", "-", text)
    return text.strip("-")


def file_anchors(path: Path) -> set[str]:
    anchors: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            anchors.add(slugify(line))
    return anchors


def check_citation(
    root: Path, citation: str, cache: dict[Path, set[str]], require_anchor: bool
) -> None:
    if citation == "-":
        return
    if "#" not in citation:
        if not require_anchor:
            if not (root / citation).is_file():
                raise Inconsistency(f"file '{citation}' does not exist")
            return
        raise Inconsistency(f"authority '{citation}' has no '#anchor'")
    rel, anchor = citation.split("#", 1)
    path = root / rel
    if not path.is_file():
        raise Inconsistency(f"authority file '{rel}' does not exist")
    anchors = cache.get(path)
    if anchors is None:
        anchors = file_anchors(path)
        cache[path] = anchors
    if anchor not in anchors:
        raise Inconsistency(f"authority anchor '{anchor}' not found in {rel}")


def run(root: Path) -> None:
    spec_lines = (root / SPEC).read_text(encoding="utf-8").splitlines()
    registry_lines = (root / REGISTRY).read_text(encoding="utf-8").splitlines()
    rows = registry_rows(registry_lines)

    expected: set[str] = set()
    for heading in APPENDIX_TABLES:
        expected |= appendix_surfaces(spec_lines, heading)
    registered = registry_surfaces(rows)

    missing = sorted(expected - registered)
    extra = sorted(registered - expected)
    if missing or extra:
        raise Inconsistency(
            f"registry does not match the spec appendix: "
            f"missing {missing}, unexpected {extra}"
        )

    cache: dict[Path, set[str]] = {}
    for row in rows:
        check_citation(root, row[2].strip(), cache, require_anchor=True)
        check_citation(root, row[4].strip(), cache, require_anchor=False)

    appendix_only = [row[0].strip() for row in rows if row[5].strip() == "appendix"]
    print(f"concepts: {len(rows)} registered, {len(expected)} appendix surfaces")
    print(f"consolidation gap: {len(appendix_only)} concepts are appendix-only")
    for concept in appendix_only:
        print(f"  - {concept}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root to check (defaults to the script's repository)",
    )
    args = parser.parse_args()
    try:
        run(args.root.resolve())
    except Inconsistency as error:
        print(f"concept registry error: {error}", file=sys.stderr)
        return 1
    print(f"concept registry valid: {args.root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
