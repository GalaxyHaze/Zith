#!/usr/bin/env python3
"""Build estatico do site Zith.

Expande os parciais do header/footer, resolve {{baseurl}} e escreve o
resultado em _site/. Sem dependencias externas.

Uso:
    python3 tools/build.py
    python3 tools/build.py --baseurl /Zith-website

Nas paginas-fonte:
    <!-- include:header active="docs" -->
    <!-- include:footer -->
    {{baseurl}}        -> prefixo do site (vazio em local)
"""
from __future__ import annotations

import argparse
import re
import shutil
from pathlib import Path

INCLUDE = re.compile(r"<!--\s*include:\s*([\w-]+)\s*(.*?)-->", re.S)
ARG = re.compile(r'([\w-]+)\s*=\s*"([^"]*)"')
ACTIVE = re.compile(r"\{\{\s*active:([\w-]+)\s*\}\}")
VAR = re.compile(r"\{\{\s*([\w-]+)\s*\}\}")

# Nunca publicados: ferramentas, parciais, notas internas e fontes dos docs.
SKIP_DIRS = {
    ".git", ".github", ".vscode", ".idea", ".agents", ".codex",
    "tools", "partials", "notes", "_site", "node_modules", "__pycache__",
    "tests", "worker",
    "agents", "pages",
}
SKIP_FILES = {
    ".gitignore", ".DS_Store", "Thumbs.db",
    "README.md", "GUIDE.md", "CLAUDE.md", "Makefile",
}
SKIP_SUFFIXES = {".md", ".pyc"}

# Posts do Dev Log: paginas de conteudo em about/posts/ que nao declaram
# includes; o build injeta-lhes o cromo partilhado (header/footer) e o CSS.
POSTS_DIR = ("about", "posts")
POST_CSS = (
    "fonts.css", "tokens.css", "base.css", "header.css", "footer.css", "devlog.css",
)
DOCS_ROUTES = {
    "index.html": "neutral",
    "zith/index.html": "zith",
    "zith--/index.html": "zith--",
}


def expand(text: str, partials: Path, baseurl: str, meta: dict[str, str]) -> str:
    values = {**meta, "baseurl": baseurl}

    def expand_include(match: re.Match) -> str:
        name = match.group(1)
        args = dict(ARG.findall(match.group(2)))
        local = {**values, **args}
        path = partials / f"{name}.html"
        if not path.is_file():
            raise SystemExit(f"build: include '{name}' nao encontrado em {path}")
        partial = path.read_text(encoding="utf-8")
        partial = ACTIVE.sub(
            lambda a: " is-active" if a.group(1) == local.get("active", "") else "",
            partial,
        )
        return VAR.sub(lambda v: local.get(v.group(1), v.group(0)), partial)

    while INCLUDE.search(text):
        text = INCLUDE.sub(expand_include, text)
    return VAR.sub(lambda v: values.get(v.group(1), v.group(0)), text)


def inject_post_chrome(text: str, partials: Path, baseurl: str) -> str:
    """Da o cromo partilhado aos posts do Dev Log (paginas de conteudo)."""
    if "include:header" in text:
        return text
    head = expand("<!-- include:head -->", partials, baseurl, {})
    links = "".join(
        f'\n    <link rel="stylesheet" href="{baseurl}/css/{name}">' for name in POST_CSS
    )
    text = text.replace("</head>", head + "\n" + links + "\n</head>", 1)
    header = expand('<!-- include:header active="devlog" -->', partials, baseurl, {})
    footer = expand("<!-- include:footer -->", partials, baseurl, {})
    text = re.sub(r"<body[^>]*>", lambda m: m.group(0) + "\n" + header, text, count=1)
    text = text.replace("</body>", footer + "\n</body>", 1)
    return text


def build(src: Path, out: Path, partials: Path, baseurl: str) -> int:
    if not partials.is_dir():
        raise SystemExit(f"build: falta a pasta de parciais: {partials}")
    if out.exists():
        shutil.rmtree(out)

    pages = 0
    for path in sorted(src.rglob("*")):
        rel = path.relative_to(src)
        if any(part in SKIP_DIRS for part in rel.parts):
            continue
        dst = out / rel
        if path.is_dir():
            dst.mkdir(parents=True, exist_ok=True)
            continue
        if path.name in SKIP_FILES or path.suffix.lower() in SKIP_SUFFIXES:
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        if path.suffix.lower() == ".html":
            text = expand(path.read_text(encoding="utf-8"), partials, baseurl, {})
            if rel.parts[:2] == POSTS_DIR:
                text = inject_post_chrome(text, partials, baseurl)
            dst.write_text(text, encoding="utf-8")
            pages += 1
        else:
            shutil.copy2(path, dst)

    docs_shell = out / "docs" / "D-home.html"
    if docs_shell.is_file():
        shell = docs_shell.read_text(encoding="utf-8")
        for route, scope in DOCS_ROUTES.items():
            route_shell = shell.replace(
                'data-docs-scope="neutral"',
                f'data-docs-scope="{scope}"',
                1,
            )
            destination = out / "docs" / route
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(route_shell, encoding="utf-8")
    return pages


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Build estatico do site Zith.")
    parser.add_argument("--src", type=Path, default=root, help="raiz das paginas-fonte")
    parser.add_argument("--out", type=Path, default=root / "_site", help="destino do build")
    parser.add_argument("--partials", type=Path, default=root / "partials", help="pasta dos parciais")
    parser.add_argument("--baseurl", default="", help="prefixo do URL do site")
    args = parser.parse_args()
    pages = build(
        args.src.resolve(),
        args.out.resolve(),
        args.partials.resolve(),
        args.baseurl.rstrip("/"),
    )
    print(f"build: {pages} pagina(s) HTML -> {args.out}")


if __name__ == "__main__":
    main()
