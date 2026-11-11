#!/usr/bin/env python3
"""Check the documentation deltas under docs/deltas/.

A delta is an isolated playground for a change to a canonical document. The
author drafts the change in the delta, compares it against the target section,
and, when it is ready, imports it into the document and archives the delta.
The workflow is described in docs/deltas/README.md.

This check keeps the delta set honest. It fails when:

1. a delta has no Status, or a Status outside the known set;
2. an active delta (draft, review) is not under docs/deltas/, or an archived
   delta (imported, dropped) is not under docs/deltas/archive/;
3. a Target does not resolve to a real file and heading, except that a dropped
   delta may have no target;
4. a delta is missing the Status or Target header the workflow needs.

It is offline: it reads only files in the tree, so it can run in CI next to
check-docs-consistency.py and check-concepts.py. The `--show` mode prints the
current text of a delta's target section, so the playground and the canonical
text can be read side by side.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


DELTAS = "docs/deltas"
ARCHIVE = "docs/deltas/archive"
SKIP = {"README.md", "TEMPLATE.md"}
ACTIVE = {"draft", "review"}
ARCHIVED = {"imported", "dropped"}


class Inconsistency(ValueError):
    """Raised when a delta breaks the workflow contract."""


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


def heading_level(line: str) -> int:
    match = re.match(r"^(#+)\s", line)
    return len(match.group(1)) if match else 0


def find_section(lines: list[str], anchor: str) -> list[str] | None:
    """Return the lines of the section whose heading slug is ``anchor``."""
    for index, line in enumerate(lines):
        if not line.startswith("#") or slugify(line) != anchor:
            continue
        level = heading_level(line)
        body = [line]
        for following in lines[index + 1 :]:
            next_level = heading_level(following)
            if next_level and next_level <= level:
                break
            body.append(following)
        return body
    return None


def parse_header(lines: list[str]) -> dict[str, str]:
    """Return the delta header fields from the first lines of the file."""
    fields: dict[str, str] = {}
    for line in lines[:20]:
        match = re.match(r"^(Status|Target|Owner|Related):\s*(.*)$", line.strip())
        if match:
            fields[match.group(1)] = match.group(2).strip()
    return fields


def resolve_target(root: Path, citation: str, cache: dict[Path, set[str]]) -> Path:
    """Verify that ``path#anchor`` resolves and return the target file path."""
    if "#" not in citation:
        raise Inconsistency(f"target '{citation}' has no '#anchor'")
    rel, anchor = citation.split("#", 1)
    path = root / rel
    if not path.is_file():
        raise Inconsistency(f"target file '{rel}' does not exist")
    anchors = cache.get(path)
    if anchors is None:
        anchors = file_anchors(path)
        cache[path] = anchors
    if anchor not in anchors:
        raise Inconsistency(f"target anchor '{anchor}' not found in {rel}")
    return path


def check_delta(
    root: Path,
    path: Path,
    cache: dict[Path, set[str]],
    active: list[str],
    archived: list[str],
) -> None:
    rel = path.relative_to(root)
    lines = path.read_text(encoding="utf-8").splitlines()
    header = parse_header(lines)

    status = header.get("Status")
    if not status:
        raise Inconsistency(f"{rel}: missing 'Status:' header")
    if status not in ACTIVE | ARCHIVED:
        raise Inconsistency(
            f"{rel}: unknown status '{status}' "
            f"(expected one of {sorted(ACTIVE | ARCHIVED)})"
        )

    is_archived = path.parent == root / ARCHIVE
    if status in ACTIVE and is_archived:
        raise Inconsistency(
            f"{rel}: status '{status}' is active and must live under {DELTAS}/"
        )
    if status in ARCHIVED and not is_archived:
        raise Inconsistency(
            f"{rel}: status '{status}' is archived and must live under {ARCHIVE}/"
        )

    target = header.get("Target")
    if not target:
        raise Inconsistency(f"{rel}: missing 'Target:' header")
    if target == "-":
        if status != "dropped":
            raise Inconsistency(f"{rel}: only a dropped delta may have no target")
    else:
        resolve_target(root, target, cache)

    (archived if is_archived else active).append(str(rel))


def collect(root: Path, directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    return [p for p in sorted(directory.glob("*.md")) if p.name not in SKIP]


def run(root: Path) -> None:
    base = root / DELTAS
    if not base.is_dir():
        raise Inconsistency(f"{DELTAS} does not exist")

    cache: dict[Path, set[str]] = {}
    active: list[str] = []
    archived: list[str] = []
    for path in collect(root, base):
        check_delta(root, path, cache, active, archived)
    for path in collect(root, root / ARCHIVE):
        check_delta(root, path, cache, active, archived)

    print(f"deltas: {len(active)} active, {len(archived)} archived")
    for name in active:
        print(f"  - {name}")


def show(root: Path, delta: str) -> None:
    path = root / delta
    if not path.is_file():
        raise Inconsistency(f"delta '{delta}' does not exist")
    header = parse_header(path.read_text(encoding="utf-8").splitlines())
    target = header.get("Target", "-")
    if target == "-":
        raise Inconsistency(f"{delta}: has no target to show")
    rel, anchor = target.split("#", 1)
    target_path = root / rel
    if not target_path.is_file():
        raise Inconsistency(f"target file '{rel}' does not exist")
    section = find_section(target_path.read_text(encoding="utf-8").splitlines(), anchor)
    if section is None:
        raise Inconsistency(f"target anchor '{anchor}' not found in {rel}")
    print(f"# target: {target}")
    print()
    print("\n".join(section).rstrip())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root to check (defaults to the script's repository)",
    )
    parser.add_argument(
        "--show",
        metavar="DELTA",
        help="print the target section of a delta instead of checking",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.show:
            show(root, args.show)
        else:
            run(root)
    except Inconsistency as error:
        print(f"delta error: {error}", file=sys.stderr)
        return 1
    if not args.show:
        print(f"deltas valid: {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
