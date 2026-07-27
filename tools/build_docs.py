#!/usr/bin/env python3
"""Build the static Zith documentation fragments from Markdown sources.

The published HTML stays in version control so the site can be served as
static files. Editorial content lives in docs/pages; specification content is
imported from the adjacent compiler checkout during every synchronization.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import posixpath
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
PAGES = ROOT / "docs" / "pages"
OUTPUT = ROOT / "html" / "documentation"
TREE = ROOT / "json" / "tree.json"
MANIFEST = ROOT / "json" / "spec-manifest.json"
SPEC_DIR = ROOT.parent / "Zith" / "docs"

PROTECTED_PATHS = [
    "D-home.html",
]
@dataclass(frozen=True)
class Page:
    id: str
    title: str
    section: str
    output: str
    aliases: tuple[str, ...]
    kind: str
    body: str
    source: Path
    navigation: bool = True


SPEC_CHAPTERS = [
    ("02-module-system.md", "Module System"),
    ("03-type-system.md", "Type System"),
    ("04-traits-interfaces.md", "Traits & Interfaces"),
    ("05-functions.md", "Functions"),
    ("06-mutability-bindings.md", "Bindings"),
    ("07-memory-model.md", "Memory Model"),
    ("08-error-handling.md", "Errors"),
    ("09-control-flow.md", "Control Flow"),
    ("10-concurrency.md", "Concurrency"),
    ("11-comptime.md", "Comptime"),
    ("12-assets.md", "Assets"),
    ("13-raw-unsafe.md", "Raw & Unsafe"),
    ("14-polymorphism.md", "Polymorphism"),
    ("15-macros.md", "Macros"),
    ("16-words.md", "Words"),
    ("17-contexts.md", "Contexts"),
    ("18-c-interop.md", "C Interop"),
    ("19-project-config.md", "Project Configuration"),
    ("20-standard-library.md", "Standard Library"),
    ("21-best-practices.md", "Best Practices"),
]

LEGACY_ANCHORS = {
    ("04-traits-interfaces.md", "44-capabilities--built-in-reference"): ("43-capabilities",),
}


def slug(text: str) -> str:
    value = re.sub(r"<[^>]+>", "", text).strip().lower()
    value = re.sub(r"[^a-z0-9\s-]", "", value)
    value = re.sub(r"\s", "-", value)
    return value.strip("-") or "section"


def parse_front_matter(path: Path) -> Page:
    text = path.read_text(encoding="utf-8")
    if not text.startswith("---\n"):
        raise ValueError(f"{path}: missing front matter")
    end = text.find("\n---\n", 4)
    if end < 0:
        raise ValueError(f"{path}: unclosed front matter")
    fields: dict[str, str] = {}
    for line in text[4:end].splitlines():
        if not line.strip():
            continue
        key, sep, value = line.partition(":")
        if not sep:
            raise ValueError(f"{path}: malformed metadata line: {line}")
        fields[key.strip()] = value.strip()
    required = ("id", "title", "section", "output", "aliases", "kind")
    missing = [key for key in required if key not in fields]
    if missing:
        raise ValueError(f"{path}: missing metadata: {', '.join(missing)}")
    aliases = tuple(item.strip() for item in fields["aliases"].split(",") if item.strip())
    navigation = fields.get("navigation", "true").lower() != "false"
    return Page(
        id=fields["id"],
        title=fields["title"],
        section=fields["section"],
        output=fields["output"],
        aliases=aliases,
        kind=fields["kind"],
        body=text[end + 5 :].strip() + "\n",
        source=path,
        navigation=navigation,
    )


def imported_pages() -> tuple[list[Page], dict[str, str]]:
    if not SPEC_DIR.is_dir():
        raise FileNotFoundError(
            f"Specification directory is unavailable: {SPEC_DIR}. "
            "A documentation synchronization requires ../Zith/docs."
        )
    pages: list[Page] = []
    hashes: dict[str, str] = {}
    imports = [("Zith-spec.md", "Specification", "reference/D-specification.html")]
    imports.append(("impl-status.md", "Implementation Status", "reference/D-implementation-status.html"))
    imports.extend(
        (filename, title, f"reference/D-{filename[3:-3]}.html")
        for filename, title in SPEC_CHAPTERS
    )
    for filename, title, output in imports:
        source = SPEC_DIR / filename
        if not source.is_file():
            raise FileNotFoundError(f"Missing canonical specification source: {source}")
        body = source.read_text(encoding="utf-8")
        hashes[filename] = hashlib.sha256(body.encode("utf-8")).hexdigest()
        page_id = "reference-" + ("implementation-status" if filename == "impl-status.md"
                                  else "specification" if filename == "Zith-spec.md"
                                  else filename[:-3])
        aliases: tuple[str, ...] = ()
        if filename == "Zith-spec.md":
            aliases = ("spec/D-overview.html",)
        elif filename == "impl-status.md":
            aliases = ("spec/D-impl-status.html",)
        else:
            aliases = (f"spec/D-{filename[3:-3]}.html",)
        pages.append(Page(page_id, title, "Language Reference", output, aliases,
                          "spec", body, source))
    return pages, hashes


def inline(text: str, page: Page, outputs: dict[str, str], source_map: dict[str, str]) -> str:
    escaped = html.escape(text, quote=False)
    escaped = re.sub(r"`([^`]+)`", r"<code>\1</code>", escaped)

    def link(match: re.Match[str]) -> str:
        label, target = match.group(1), html.unescape(match.group(2))
        if target.startswith("http://") or target.startswith("https://") or target.startswith("mailto:"):
            return f'<a href="{html.escape(target, quote=True)}">{label}</a>'
        if target.startswith("#"):
            return f'<a href="{html.escape(target, quote=True)}">{label}</a>'
        if target.startswith("doc:"):
            target_id, _, anchor = target[4:].partition("#")
            if target_id not in outputs:
                raise ValueError(f"{page.source}: unknown document id '{target_id}'")
            href = relative_href(page.output, outputs[target_id]) + (f"#{anchor}" if anchor else "")
            return f'<a href="{href}">{label}</a>'
        file_part, sep, anchor = target.partition("#")
        resolved = (page.source.parent / file_part).resolve()
        if resolved in source_map:
            href = relative_href(page.output, source_map[resolved])
            return f'<a href="{href}{sep}{anchor if sep else ""}">{label}</a>'
        return f'<a href="{html.escape(target, quote=True)}">{label}</a>'

    escaped = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, escaped)
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", escaped)
    escaped = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"<em>\1</em>", escaped)
    return escaped


def relative_href(source_output: str, target_output: str) -> str:
    source_dir = Path(source_output).parent
    target = Path(target_output)
    return "./" + str(Path(reldir(target, source_dir))).replace("\\", "/")


def reldir(target: Path, source_dir: Path) -> Path:
    # pathlib has no relative-path API that works across arbitrary paths.
    import os
    return Path(os.path.relpath(target, source_dir))


def is_table_separator(line: str) -> bool:
    return bool(re.match(r"^\s*\|?[\s:|-]+\|[\s:| -]*$", line))


def table_html(lines: list[str], page: Page, outputs: dict[str, str], source_map: dict[str, str]) -> str:
    rows = [[cell.strip() for cell in line.strip().strip("|").split("|")]
            for line in lines if not is_table_separator(line)]
    if not rows:
        return ""
    rendered = ["<table>", "  <thead>", "    <tr>"]
    rendered.extend(f"      <th>{inline(cell, page, outputs, source_map)}</th>" for cell in rows[0])
    rendered += ["    </tr>", "  </thead>"]
    if len(rows) > 1:
        rendered += ["  <tbody>"]
        for row in rows[1:]:
            rendered.append("    <tr>")
            rendered.extend(f"      <td>{inline(cell, page, outputs, source_map)}</td>" for cell in row)
            rendered.append("    </tr>")
        rendered += ["  </tbody>"]
    rendered.append("</table>")
    return "\n".join(rendered)


def markdown_html(page: Page, outputs: dict[str, str], source_map: dict[str, str]) -> str:
    lines = page.body.splitlines()
    result: list[str] = []
    index = 0
    heading_count = 0
    seen_ids: dict[str, int] = {}

    def heading(level: int, text: str) -> None:
        nonlocal heading_count
        anchor = slug(text)
        occurrence = seen_ids.get(anchor, 0)
        seen_ids[anchor] = occurrence + 1
        if occurrence:
            anchor = f"{anchor}-{occurrence + 1}"
        if level == 1:
            heading_count += 1
        result.append(f"<h{level} id=\"{anchor}\">{inline(text, page, outputs, source_map)}</h{level}>")
        for legacy in LEGACY_ANCHORS.get((page.source.name, anchor), ()):
            result.append(f'<span id="{legacy}" aria-hidden="true"></span>')

    while index < len(lines):
        line = lines[index]
        if not line.strip():
            index += 1
            continue
        fence = re.match(r"^```([^`]*)$", line)
        if fence:
            language = fence.group(1).strip()
            index += 1
            code: list[str] = []
            while index < len(lines) and not lines[index].startswith("```"):
                code.append(lines[index])
                index += 1
            if index == len(lines):
                raise ValueError(f"{page.source}: unclosed code fence")
            cls = f' class="language-{html.escape(language, quote=True)}"' if language else ""
            result.append(f"<pre><code{cls}>{html.escape(chr(10).join(code))}</code></pre>")
            index += 1
            continue
        match = re.match(r"^(#{1,6})\s+(.+?)\s*$", line)
        if match:
            level = len(match.group(1))
            text = match.group(2)
            # Imported chapters start with H2 because they were included in a former monolith.
            if page.kind == "spec" and heading_count == 0 and level > 1:
                level = 1
            heading(level, text)
            index += 1
            continue
        if line.startswith("> "):
            quote: list[str] = []
            while index < len(lines) and lines[index].startswith("> "):
                quote.append(lines[index][2:])
                index += 1
            result.append(f"<blockquote><p>{inline(' '.join(quote), page, outputs, source_map)}</p></blockquote>")
            continue
        if line.startswith("|"):
            table: list[str] = []
            while index < len(lines) and lines[index].startswith("|"):
                table.append(lines[index])
                index += 1
            result.append(table_html(table, page, outputs, source_map))
            continue
        list_match = re.match(r"^(\s*)([-*+]|\d+\.)\s+(.+)$", line)
        if list_match:
            ordered = list_match.group(2).endswith(".")
            tag = "ol" if ordered else "ul"
            entries: list[str] = []
            while index < len(lines):
                item_match = re.match(r"^\s*([-*+]|\d+\.)\s+(.+)$", lines[index])
                if not item_match or item_match.group(1).endswith(".") != ordered:
                    break
                entries.append(item_match.group(2))
                index += 1
            result.append(f"<{tag}>")
            result.extend(f"  <li>{inline(entry, page, outputs, source_map)}</li>" for entry in entries)
            result.append(f"</{tag}>")
            continue
        if line == "---":
            result.append("<hr>")
            index += 1
            continue
        paragraph = [line.strip()]
        index += 1
        while index < len(lines) and lines[index].strip() and not re.match(
            r"^(#{1,6}\s|```|> |\||[-*+]\s|\d+\. |---$)", lines[index]
        ):
            paragraph.append(lines[index].strip())
            index += 1
        result.append(f"<p>{inline(' '.join(paragraph), page, outputs, source_map)}</p>")

    if heading_count != 1:
        raise ValueError(f"{page.source}: generated page must contain exactly one h1 (found {heading_count})")
    return "\n".join(result) + "\n"


def nav_item(title: str, page_id: str, children: Iterable[dict] = ()) -> dict:
    return {"title": title, "link": f"./{PAGES_BY_ID[page_id].output}", "children": list(children)}


PAGES_BY_ID: dict[str, Page] = {}


def build_tree() -> list[dict]:
    guide = [
        ("Syntax", "guide-syntax"), ("Bindings", "guide-bindings"), ("Types", "guide-types"),
        ("Functions", "guide-functions"), ("Modules", "guide-modules"),
        ("Control Flow", "guide-control-flow"), ("Generics", "guide-generics"),
        ("Memory Model", "guide-memory-model"), ("Errors", "guide-errors"),
        ("Traits & Interfaces", "guide-traits-interfaces"), ("Comptime", "guide-comptime"),
        ("Contexts, Words & Macros", "guide-contexts-words-macros"),
        ("Concurrency", "guide-concurrency"), ("C Interop", "guide-c-interop"),
        ("Raw & Unsafe", "guide-raw-unsafe"),
    ]
    reference = [("Implementation Status", "reference-implementation-status")]
    reference += [(title, "reference-" + filename[:-3]) for filename, title in SPEC_CHAPTERS]
    cli = [(f"zithc {command}", f"cli-{command}") for command in
           ("build", "run", "check", "fmt", "create", "clean", "execute")]
    return [{
        "title": "Home", "link": "../home.html", "children": [
            nav_item("Getting Started", "getting-started-quick-start", [
                nav_item("Introduction", "getting-started-introduction"),
                nav_item("Installation", "getting-started-installation"),
                nav_item("Why Zith", "getting-started-why-zith"),
            ]),
            nav_item("Language Guide", "guide-overview", [nav_item(title, page_id) for title, page_id in guide]),
            nav_item("Language Reference", "reference-specification",
                     [nav_item(title, page_id) for title, page_id in reference]),
            nav_item("CLI Reference", "cli-overview", [nav_item(title, page_id) for title, page_id in cli]),
            nav_item("Project", "project-overview"),
            nav_item("FAQ", "faq-overview"),
            nav_item("Community", "community-overview"),
        ],
    }]


def legacy_aliases(pages: list[Page]) -> dict[str, str]:
    aliases: dict[str, str] = {}
    for page in pages:
        for alias in page.aliases:
            if alias != page.output:
                aliases[alias] = page.output
    extra = {
        "D-project-overview.html": "project/D-overview.html",
        "D-roadmap.html": "project/D-overview.html",
        "D-reference-stdlib.html": "reference/D-standard-library.html",
        "D-language-spec-core-topics.html": "reference/D-specification.html",
        "D-dev-wasm-build.html": "cli/D-build.html",
        "language/D-ecs.html": "guide/D-overview.html",
        "language/D-intrinsics.html": "guide/D-comptime.html",
        "language/D-packs.html": "guide/D-bindings.html",
        "language/D-ownership.html": "guide/D-memory-model.html",
        "language/D-memory.html": "guide/D-memory-model.html",
        "language/D-best-practices.html": "reference/D-best-practices.html",
        "advanced/D-overview.html": "guide/D-overview.html",
        "advanced/D-how-to-use.html": "guide/D-overview.html",
        "advanced/D-traits.html": "guide/D-traits-interfaces.html",
        "advanced/D-generics-deep.html": "guide/D-generics.html",
        "advanced/D-metaprogramming.html": "guide/D-comptime.html",
        "advanced/D-macros.html": "guide/D-contexts-words-macros.html",
        "advanced/D-data-structures.html": "guide/D-types.html",
        "advanced/D-unsafe.html": "guide/D-raw-unsafe.html",
        "advanced/D-raw-pointers.html": "guide/D-raw-unsafe.html",
        "cli/D-compile.html": "cli/D-build.html",
        "cli/D-new.html": "cli/D-create.html",
        "cli/D-docs.html": "cli/D-overview.html",
        "cli/D-flags.html": "cli/D-overview.html",
        "cli/D-repl.html": "cli/D-overview.html",
        "faq/D-philosophy.html": "faq/D-overview.html",
        "faq/D-security.html": "faq/D-overview.html",
        "faq/D-rust-comparison.html": "faq/D-overview.html",
        "faq/D-use-cases.html": "faq/D-overview.html",
        "community/D-chat.html": "community/D-overview.html",
        "community/D-contributing.html": "community/D-overview.html",
        "community/D-code-of-conduct.html": "community/D-overview.html",
    }
    aliases.update(extra)
    return aliases


def alias_html(alias: str, target: str) -> str:
    href = relative_href(alias, target)
    return (
        '<div class="callout callout-info"><p><strong>Moved:</strong> This legacy URL is '
        f'maintained for compatibility. Continue to <a href="{href}">the current page</a>.</p></div>\n'
        "<h1>Documentation moved</h1>\n"
        f'<p>This page is now maintained at <a href="{href}">{html.escape(target)}</a>.</p>\n'
    )


def status_notice(page: Page) -> str:
    href = relative_href(page.output, "reference/D-implementation-status.html")
    return (
        '<div class="callout callout-warning"><p><strong>Draft / Experimental:</strong> '
        'Zith documentation describes a language under active development. Check '
        f'<a href="{href}">Implementation Status</a> before relying on a feature.</p></div>'
    )


def expected_files(pages: list[Page], spec_hashes: dict[str, str]) -> tuple[dict[Path, str], dict]:
    global PAGES_BY_ID
    PAGES_BY_ID = {page.id: page for page in pages}
    if len(PAGES_BY_ID) != len(pages):
        raise ValueError("Duplicate page id")
    outputs = {page.id: page.output for page in pages}
    if len(set(outputs.values())) != len(outputs):
        raise ValueError("Duplicate output path")
    source_map = {page.source.resolve(): page.output for page in pages}
    files: dict[Path, str] = {}
    for page in pages:
        fragment = markdown_html(page, outputs, source_map)
        fragment = status_notice(page) + "\n" + fragment
        files[OUTPUT / page.output] = fragment
    aliases = legacy_aliases(pages)
    for alias, target in aliases.items():
        if target not in outputs.values():
            raise ValueError(f"Alias {alias} points to unknown output {target}")
        alias_path = OUTPUT / alias
        if alias_path in files:
            raise ValueError(f"Alias conflicts with canonical output: {alias}")
        files[alias_path] = alias_html(alias, target)
    for protected in PROTECTED_PATHS:
        protected_path = OUTPUT / protected
        if protected_path in files:
            raise ValueError(f"Output or alias would overwrite protected file: {protected}")
    tree = {"version": 2, "navigation": build_tree(), "aliases": {
        f"./{alias}": f"./{target}" for alias, target in sorted(aliases.items())
    }}
    manifest = {
        "version": 1,
        "spec_directory": "../Zith/docs",
        "sources": spec_hashes,
        "outputs": {page.output: str(page.source.relative_to(SPEC_DIR)) for page in pages if page.kind == "spec"},
    }
    files[TREE] = json.dumps(tree, indent=2) + "\n"
    files[MANIFEST] = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    return files, tree


def validate(files: dict[Path, str], tree: dict) -> list[str]:
    errors: list[str] = []
    outputs = {str(path.relative_to(OUTPUT)): body for path, body in files.items()
               if path.is_relative_to(OUTPUT)}
    nav_links: list[str] = []

    def collect(items: list[dict]) -> None:
        for item in items:
            link = item.get("link", "")
            if link and link != "../home.html":
                nav_links.append(link.removeprefix("./"))
            collect(item.get("children", []))
    collect(tree["navigation"])
    for link in nav_links:
        if link not in outputs:
            errors.append(f"navigation link does not resolve: {link}")
    output_ids = {
        output: set(re.findall(r'\sid="([^"]+)"', body))
        for output, body in outputs.items()
    }
    for output, body in outputs.items():
        if body.count("<h1") != 1:
            errors.append(f"{output}: expected one h1")
        if "under construction" in body.lower() or "content coming soon" in body.lower():
            errors.append(f"{output}: placeholder content is not allowed")
        if re.search(r"(?<!c)\bzith (?:build|compile|new|run|check|fmt|clean)\b", body):
            errors.append(f"{output}: removed CLI nomenclature found")
        for href in re.findall(r'href="([^"]+)"', body):
            if href.startswith(("http://", "https://", "mailto:")):
                continue
            file_part, _, anchor = href.partition("#")
            if not file_part:
                if anchor and anchor not in output_ids[output]:
                    errors.append(f"{output}: missing local anchor #{anchor}")
                continue
            normalized = posixpath.normpath(
                posixpath.join(posixpath.dirname(output), file_part.removeprefix("./"))
            )
            if normalized == ".." or normalized.startswith("../"):
                errors.append(f"{output}: link escapes documentation root: {href}")
                continue
            if normalized not in outputs:
                errors.append(f"{output}: broken internal link {href}")
            elif anchor and anchor not in output_ids[normalized]:
                errors.append(f"{output}: {href} points to a missing anchor")
    return errors


def run(check: bool) -> int:
    local_pages = [parse_front_matter(path) for path in sorted(PAGES.rglob("*.md"))]
    spec_pages, hashes = imported_pages()
    files, tree = expected_files(local_pages + spec_pages, hashes)
    errors = validate(files, tree)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    stale = [path for path, content in files.items()
             if not path.is_file() or path.read_text(encoding="utf-8") != content]
    expected_html = {path for path in files if path.is_relative_to(OUTPUT)}
    existing_html = set(OUTPUT.rglob("D-*.html"))
    protected_set = {(OUTPUT / p).resolve() for p in PROTECTED_PATHS}
    orphaned = sorted(existing_html - expected_html)
    orphaned = [p for p in orphaned if p.resolve() not in protected_set]
    if check:
        if stale or orphaned:
            print("Documentation output is stale. Run: python3 tools/build_docs.py", file=sys.stderr)
            for path in stale:
                print(f"  stale: {path.relative_to(ROOT)}", file=sys.stderr)
            for path in orphaned:
                print(f"  orphaned: {path.relative_to(ROOT)}", file=sys.stderr)
            return 1
        print(f"Documentation is current ({len(expected_html)} fragments).")
        return 0
    for path, content in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
    for path in orphaned:
        path.unlink()
    print(f"Built {len(expected_html)} documentation fragments.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail when generated output is stale")
    args = parser.parse_args()
    try:
        return run(args.check)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
