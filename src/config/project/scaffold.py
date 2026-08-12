#!/usr/bin/env python3
"""Create project tree blueprints from scaffold.toml."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


class ScaffoldError(ValueError):
    def __init__(self, path: Path, line_no: int, reason: str) -> None:
        super().__init__(f"{path}:{line_no}: {reason}")
        self.path = path
        self.line_no = line_no
        self.reason = reason


def read_toml(path: Path) -> dict[str, object]:
    result: dict[str, object] = {}
    current: dict[str, object] | None = None
    in_literal = False
    literal_key: str | None = None
    literal_lines: list[str] = []

    def flush_literal() -> None:
        nonlocal in_literal, literal_key
        if in_literal and current is not None and literal_key is not None:
            current[literal_key] = "\n".join(literal_lines) + "\n"
            in_literal = False
            literal_key = None
            literal_lines.clear()

    lines = path.read_text(encoding="utf-8").splitlines()
    for line_no, raw in enumerate(lines, start=1):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if in_literal:
            if stripped == '"""':
                flush_literal()
                continue
            literal_lines.append(raw)
            continue
        match = re.fullmatch(r"\[([^\]]+)\]", stripped)
        if match:
            flush_literal()
            section = match.group(1).strip()
            current = _make_section_path(result, section, path, line_no)
            continue
        if current is None:
            raise ScaffoldError(path, line_no, f"chave fora de seccao: {raw!r}")
        if stripped.startswith('"""'):
            if current is None:
                raise ScaffoldError(path, line_no, "literal fora de seccao")
            in_literal = True
            literal_key = stripped[3:].strip()
            literal_lines = []
            continue
        if "=" not in stripped:
            raise ScaffoldError(path, line_no, f"chave sem valor: {raw!r}")
        key, value = (part.strip() for part in stripped.split("=", 1))
        current[key] = parse_value(value, path, line_no)
    flush_literal()
    return result


def _make_section_path(
    root: dict[str, object], section: str, path: Path, line_no: int
) -> dict[str, object]:
    target: dict[str, object] = root
    dot = section.split(".")
    for index, part in enumerate(dot):
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", part):
            raise ScaffoldError(path, line_no, f"seccao invalida: {section!r}")
        child = target.get(part)
        if child is None and index == len(dot) - 1:
            child = {}
            target[part] = child
        elif not isinstance(child, dict) or (index != len(dot) - 1 and not isinstance(child, dict)):
            raise ScaffoldError(path, line_no, f"seccao repetida/sobreposta: {section!r}")
        target = child
    return target


def parse_value(raw: str, path: Path, line_no: int) -> object:
    raw = raw.strip()
    if raw.startswith('"') and raw.endswith('"') and len(raw) >= 2:
        import json

        try:
            return json.loads(raw)
        except ValueError as exc:
            raise ScaffoldError(path, line_no, f"string invalida: {raw!r}") from exc
    if raw.startswith("[") and raw.endswith("]"):
        if not raw[1:-1].strip():
            return []
        items = [part.strip() for part in raw[1:-1].split(",") if part.strip()]
        return [parse_value(item, path, line_no) for item in items]
    if raw.lower() in {"true", "false"}:
        return raw.lower() == "true"
    raise ScaffoldError(path, line_no, f"valor TOML invalido: {raw!r}")


def blueprint_names(data: dict[str, object]) -> list[str]:
    blueprints = data.get("blueprints")
    if not isinstance(blueprints, dict):
        raise ScaffoldError(Path("<scaffold>"), 1, "falta [blueprints]")
    return [str(name) for name in blueprints if name != "files"]


def resolve_blueprint(data: dict[str, object], name: str) -> dict[str, dict[str, str]]:
    blueprints = data.get("blueprints")
    if not isinstance(blueprints, dict):
        raise ScaffoldError(Path("<scaffold>"), 1, "falta [blueprints]")
    entry = blueprints.get(name)
    if isinstance(entry, str):
        name = entry
        entry = blueprints.get(name)
    if not isinstance(entry, dict):
        raise ScaffoldError(Path("<scaffold>"), 1, f"blueprint desconhecido: {name!r}")
    files = entry.get("files")
    if not isinstance(files, dict):
        raise ScaffoldError(Path("<scaffold>"), 1, f"blueprint sem ficheiros: {name!r}")
    result: dict[str, dict[str, str]] = {}
    for key, value in files.items():
        if not isinstance(key, str) or not isinstance(value, str):
            raise ScaffoldError(Path("<scaffold>"), 1, "ficheiro blueprint invalido")
        result[key] = {"content": value}
    return result


def apply_text(template: str, values: dict[str, str]) -> str:
    def replacer(match: re.Match[str]) -> str:
        name = match.group(1)
        return values.get(name, match.group(0))

    return re.sub(r"\{([A-Za-z_][A-Za-z0-9_]*)\}", replacer, template)


def scaffold(data: dict[str, object], out_dir: Path, blueprint: str, project: str) -> None:
    files = resolve_blueprint(data, blueprint)
    out_dir.mkdir(parents=True, exist_ok=True)
    for rel, file_info in files.items():
        target = out_dir / rel
        if target.exists():
            raise ScaffoldError(target, 1, f"ficheiro ja existe: {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(apply_text(file_info["content"], {"project": project}), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rules", default="src/config/project/scaffold.toml")
    parser.add_argument("--out", default=".")
    parser.add_argument("--blueprint", default="")
    parser.add_argument("--project", default="example")
    args = parser.parse_args()

    rules = Path(args.rules)
    if not rules.exists():
        print(f"scaffold rules not found: {rules}", file=sys.stderr)
        return 2
    try:
        data = read_toml(rules)
        blueprint = args.blueprint
        if not blueprint:
            mapping = data.get("blueprints")
            if isinstance(mapping, dict) and isinstance(mapping.get("zith"), str):
                blueprint = str(mapping["zith"])
            else:
                blueprint = blueprint_names(data)[0] if blueprint_names(data) else ""
        scaffold(data, Path(args.out), blueprint, args.project)
    except ScaffoldError as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
