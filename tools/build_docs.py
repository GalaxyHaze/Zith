#!/usr/bin/env python3
"""Build the static Zith documentation fragments from Markdown sources.

The published HTML stays in version control so the site can be served as
static files. Editorial content lives in docs/pages; specification content is
imported from the adjacent compiler checkout during every synchronization.

URL scheme:
    docs/D-home.html                   documentation shell
    docs/<neutral-page>.html           neutral / general docs
    docs/zith--/guide/<page>.html      Zith-- language guide
    docs/zith--/reference/<page>.html  Zith-- reference
    docs/zith/reference/<page>.html    Zith language specification
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
DEFAULT_SITE = ROOT / "_site"
OUTPUT = DEFAULT_SITE / "docs"
TREE = DEFAULT_SITE / "json" / "tree.json"
MANIFEST = DEFAULT_SITE / "json" / "spec-manifest.json"
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

SUBSET_CHAPTER = ("Zith--.md", "Zith--")

IMPORTED_LINK_ALIASES = {
    "implementation-debt.md": "reference/D-implementation-status.html",
    "plans/branch-protocol.md": "zith/reference/D-concurrency.html",
    "adr/0010-allocator-inplace-drop.md": "zith/reference/D-standard-library.html",
}

LEGACY_ANCHORS = {
    ("04-traits-interfaces.md", "44-capabilities--built-in-reference"): ("43-capabilities",),
    ("02-module-system.md", "23-namespace-access--scope-resolution"): (),
    ("08-error-handling.md", "83-propagation--fallback"): (),
    ("08-error-handling.md", "83-try-local-results-and-fallback"): (
        "83-try-propagation-and-fallback",
    ),
    ("09-control-flow.md", "94-state-functions--state-machines"): (
        "94-state-functions-and-state-machines",
    ),
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


def imported_pages(spec_dir: Path = SPEC_DIR) -> tuple[list[Page], dict[str, str]]:
    if not spec_dir.is_dir():
        raise FileNotFoundError(
            f"Specification directory is unavailable: {spec_dir}. "
            "A documentation synchronization requires ../Zith/docs."
        )
    pages: list[Page] = []
    hashes: dict[str, str] = {}
    imports = [
        ("Zith-spec.md", "Specification", "zith/reference/D-specification.html"),
        ("impl-status.md", "Implementation Status", "reference/D-implementation-status.html"),
        (SUBSET_CHAPTER[0], "Zith--", "zith--/reference/D-zith-subset.html"),
    ]
    imports.extend(
        (filename, title, f"zith/reference/D-{filename[3:-3]}.html")
        for filename, title in SPEC_CHAPTERS
    )
    for filename, title, output in imports:
        source = spec_dir / filename
        if not source.is_file():
            raise FileNotFoundError(f"Missing canonical specification source: {source}")
        body = source.read_text(encoding="utf-8")
        hashes[filename] = hashlib.sha256(body.encode("utf-8")).hexdigest()
        page_id = ("reference-zith-subset" if filename == SUBSET_CHAPTER[0]
                   else "reference-" + ("implementation-status" if filename == "impl-status.md"
                                        else "specification" if filename == "Zith-spec.md"
                                        else filename[:-3]))
        aliases: tuple[str, ...] = ()
        if filename == "Zith-spec.md":
            aliases = ("spec/D-overview.html",)
        elif filename == "impl-status.md":
            aliases = ("spec/D-impl-status.html",)
        elif filename == SUBSET_CHAPTER[0]:
            aliases = ("spec/D-zith-subset.html",)
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
        if target.startswith("route:"):
            route = target[6:]
            route_paths = {"zith": "./zith/", "zith--": "./zith--/"}
            if route not in route_paths:
                raise ValueError(f"{page.source}: unknown documentation route '{route}'")
            return (
                f'<a href="{route_paths[route]}" '
                f'data-docs-route="{route}">{label}</a>'
            )
        if target.startswith("neutral:"):
            target_id, _, anchor = target[8:].partition("#")
            if target_id not in outputs:
                raise ValueError(f"{page.source}: unknown document id '{target_id}'")
            href = f"./?page={outputs[target_id]}" + (f"#{anchor}" if anchor else "")
            return f'<a href="{href}" data-docs-route="neutral">{label}</a>'
        if target.startswith("doc:"):
            target_id, _, anchor = target[4:].partition("#")
            if target_id not in outputs:
                raise ValueError(f"{page.source}: unknown document id '{target_id}'")
            return documentation_link(page.output, outputs[target_id], label, anchor)
        file_part, sep, anchor = target.partition("#")
        resolved = (page.source.parent / file_part).resolve()
        if resolved in source_map:
            return documentation_link(
                page.output, source_map[resolved], label,
                anchor if sep else ""
            )
        if page.kind == "spec" and file_part in IMPORTED_LINK_ALIASES:
            return documentation_link(
                page.output, IMPORTED_LINK_ALIASES[file_part], label,
                anchor if sep else ""
            )
        return f'<a href="{html.escape(target, quote=True)}">{label}</a>'

    escaped = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, escaped)
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", escaped)
    escaped = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"<em>\1</em>", escaped)
    return escaped


def relative_href(source_output: str, target_output: str) -> str:
    source = source_output.replace("\\", "/")
    target = target_output.replace("\\", "/")
    source_parts = source.split("/")
    target_parts = target.split("/")
    common = 0
    for s, t in zip(source_parts, target_parts):
        if s == t:
            common += 1
        else:
            break
    up = max(0, len(source_parts) - common - 1)
    prefix = ("../" * up) if up > 0 else "./"
    suffix = "/".join(target_parts[common:])
    if not suffix:
        suffix = posixpath.basename(target)
    return prefix + suffix


def documentation_link(
    source_output: str,
    target_output: str,
    label: str,
    anchor: str = "",
) -> str:
    source_scope = ("zith--" if source_output.startswith("zith--/") else
                    "zith" if source_output.startswith("zith/") else "neutral")
    target_scope = ("zith--" if target_output.startswith("zith--/") else
                    "zith" if target_output.startswith("zith/") else "neutral")

    if source_scope != target_scope:
        if target_scope == "neutral":
            href = f"./?page={target_output}"
        elif target_output == f"{target_scope}/D-overview.html":
            href = f"./{target_scope}/"
        else:
            href = f"./{target_scope}/?page={target_output}"
        if anchor:
            href += f"#{anchor}"
        return f'<a href="{href}" data-docs-route="{target_scope}">{label}</a>'

    href = relative_href(source_output, target_output)
    if anchor:
        href += f"#{anchor}"
    return f'<a href="{href}">{label}</a>'


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
        rendered.append("  </tbody>")
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


def nav_item(title: str, page_id: str, children: Iterable[dict] = (), *, description: str = "", badge: str = "") -> dict:
    item: dict[str, object] = {"title": title, "link": f"./{PAGES_BY_ID[page_id].output}", "children": list(children)}
    if description:
        item["description"] = description
    if badge:
        item["badge"] = badge
    return item


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
    zith_reference = [("Specification", "reference-specification")]
    zith_reference += [(title, "reference-" + filename[:-3])
                       for filename, title in SPEC_CHAPTERS]
    cli = [(f"zithc {command}", f"cli-{command}") for command in
           ("build", "run", "check", "fmt", "create", "clean", "execute", "test", "deps", "docs")]
    return [{
        "title": "Home", "link": "", "children": [
            nav_item("Documentation", "docs-overview",
                     description="Compare Zith and Zith--, then choose the documentation set you need."),
            nav_item("Getting Started", "getting-started-introduction", [
                nav_item("Introduction", "getting-started-introduction"),
                nav_item("Installation", "getting-started-installation"),
                nav_item("Why Zith", "getting-started-why-zith"),
            ], description="Introduction to the project and general setup."),
            nav_item("CLI Reference", "cli-overview", [nav_item(title, page_id) for title, page_id in cli],
                     description="Reference for zithc commands: build, run, check, format, create, test, and project tooling."),
            nav_item("Project", "project-overview",
                     description="Project overview and current direction for the Zith programming language."),
            nav_item("FAQ", "faq-overview",
                     description="Frequently asked questions about Zith stability, scope, and relationship to other languages."),
            nav_item("Implementation Status", "reference-implementation-status",
                     description="Current compiler support, known restrictions, and specification-only features."),
            nav_item("Community", "community-overview",
                     description="Community resources for reporting issues and contributing to Zith documentation."),
            nav_item("Zith", "zith-overview", [
                nav_item("Overview", "zith-overview"),
                nav_item("FAQ", "zith-faq"),
                nav_item("Language Reference",
                         "reference-specification",
                         [nav_item(title, page_id) for title, page_id in zith_reference]),
            ], description="Zith language design and specification; separate from the Zith-- compiler contract."),
            nav_item("Zith--", "zith-subset-overview", [
                nav_item("Overview", "zith-subset-overview"),
                nav_item("First Program", "getting-started-quick-start"),
                nav_item("Language Guide", "guide-overview",
                         [nav_item(title, page_id) for title, page_id in guide]),
                nav_item("Reference", "zith-subset-reference"),
                nav_item("FAQ", "zith-subset-faq"),
            ], description="Zith-- language and compiler contract currently supported by zithc."),
        ],
    }]


def build_topic_equivalents() -> dict[str, dict[str, str]]:
    pairs = [
        ("zith-overview", "zith-subset-overview"),
        ("reference-specification", "zith-subset-reference"),
        ("reference-06-mutability-bindings", "guide-bindings"),
        ("reference-03-type-system", "guide-types"),
        ("reference-05-functions", "guide-functions"),
        ("reference-02-module-system", "guide-modules"),
        ("reference-09-control-flow", "guide-control-flow"),
        ("reference-14-polymorphism", "guide-generics"),
        ("reference-07-memory-model", "guide-memory-model"),
        ("reference-08-error-handling", "guide-errors"),
        ("reference-04-traits-interfaces", "guide-traits-interfaces"),
        ("reference-11-comptime", "guide-comptime"),
        ("reference-15-macros", "guide-contexts-words-macros"),
        ("reference-10-concurrency", "guide-concurrency"),
        ("reference-18-c-interop", "guide-c-interop"),
        ("reference-13-raw-unsafe", "guide-raw-unsafe"),
    ]
    equivalents: dict[str, dict[str, str]] = {
        PAGES_BY_ID["docs-overview"].output: {
            "zith": PAGES_BY_ID["zith-overview"].output,
            "zith--": PAGES_BY_ID["zith-subset-overview"].output,
        }
    }
    for zith_id, subset_id in pairs:
        zith_page = PAGES_BY_ID[zith_id].output
        subset_page = PAGES_BY_ID[subset_id].output
        equivalents.setdefault(zith_page, {})["zith--"] = subset_page
        equivalents.setdefault(subset_page, {})["zith"] = zith_page
    equivalents.setdefault(
        PAGES_BY_ID["guide-syntax"].output, {}
    )["zith"] = PAGES_BY_ID["reference-specification"].output
    return equivalents


def legacy_aliases(pages: list[Page]) -> dict[str, str]:
    aliases: dict[str, str] = {}
    for page in pages:
        for alias in page.aliases:
            if alias != page.output:
                aliases[alias] = page.output
    extra = {
        "D-project-overview.html": "project/D-overview.html",
        "D-roadmap.html": "project/D-overview.html",
        "D-reference-stdlib.html": "zith/reference/D-standard-library.html",
        "D-language-spec-core-topics.html": "zith/reference/D-specification.html",
        "D-dev-wasm-build.html": "cli/D-build.html",
        "language/D-ecs.html": "zith--/guide/D-overview.html",
        "language/D-intrinsics.html": "zith--/guide/D-comptime.html",
        "language/D-packs.html": "zith--/guide/D-bindings.html",
        "language/D-ownership.html": "zith--/guide/D-memory-model.html",
        "language/D-memory.html": "zith--/guide/D-memory-model.html",
        "language/D-best-practices.html": "zith/reference/D-best-practices.html",
        "advanced/D-overview.html": "zith--/guide/D-overview.html",
        "advanced/D-how-to-use.html": "zith--/guide/D-overview.html",
        "advanced/D-traits.html": "zith--/guide/D-traits-interfaces.html",
        "advanced/D-generics-deep.html": "zith--/guide/D-generics.html",
        "advanced/D-metaprogramming.html": "zith--/guide/D-comptime.html",
        "advanced/D-macros.html": "zith--/guide/D-contexts-words-macros.html",
        "advanced/D-data-structures.html": "zith--/guide/D-types.html",
        "advanced/D-unsafe.html": "zith--/guide/D-raw-unsafe.html",
        "advanced/D-raw-pointers.html": "zith--/guide/D-raw-unsafe.html",
        "cli/D-compile.html": "cli/D-build.html",
        "cli/D-new.html": "cli/D-create.html",
        "cli/D-flags.html": "cli/D-overview.html",
        "cli/D-repl.html": "cli/D-overview.html",
        "faq/D-philosophy.html": "faq/D-overview.html",
        "faq/D-security.html": "faq/D-overview.html",
        "faq/D-rust-comparison.html": "faq/D-overview.html",
        "faq/D-use-cases.html": "faq/D-overview.html",
        "community/D-chat.html": "community/D-overview.html",
        "community/D-contributing.html": "community/D-overview.html",
        "community/D-code-of-conduct.html": "community/D-overview.html",
        "reference/D-zith-subset.html": "zith--/reference/D-zith-subset.html",
        "zith/reference/D-implementation-status.html": "reference/D-implementation-status.html",
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


def status_notice(language: str) -> str:
    status_link = (
        '<a href="./?page=reference/D-implementation-status.html" '
        'data-docs-route="neutral">Implementation Status</a>'
    )
    if language == "zith":
        message = (
            "<strong>Language design:</strong> This page describes Zith's "
            "experimental language design, not the behavior of <code>zithc</code>. "
            f"{status_link} reports compiler support for sibling Zith-- only."
        )
    else:
        message = (
            "<strong>Beta:</strong> Zith-- is a sibling language to Zith with its "
            "own language contract. See "
            f"{status_link} for compiler support and known restrictions."
        )
    return f'<div class="callout callout-warning"><p>{message}</p></div>'


def expected_files(
    pages: list[Page],
    spec_hashes: dict[str, str],
    spec_dir: Path,
) -> tuple[dict[Path, str], dict]:
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
        if page.output.startswith("zith/"):
            fragment = status_notice("zith") + "\n" + fragment
        elif page.output.startswith("zith--/"):
            fragment = status_notice("zith--") + "\n" + fragment
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
    tree = {
        "version": 5,
        "navigation": build_tree(),
        "aliases": {f"./{alias}": f"./{target}" for alias, target in sorted(aliases.items())},
        "topicEquivalents": build_topic_equivalents(),
    }
    manifest = {
        "version": 1,
        "spec_directory": posixpath.relpath(spec_dir.as_posix(), ROOT.as_posix()),
        "sources": spec_hashes,
        "outputs": {
            page.output: page.source.relative_to(spec_dir).as_posix()
            for page in pages if page.kind == "spec"
        },
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
            if link:
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
            if href in {"./zith/", "./zith--/"}:
                continue
            if href.startswith("./?page="):
                routed_page = href[len("./?page="):].partition("#")[0]
                if routed_page not in outputs:
                    errors.append(f"{output}: broken neutral route {href}")
                continue
            language_route = re.fullmatch(
                r"\./(zith|zith--)/\?page=([^#]+)(?:#(.*))?", href
            )
            if language_route:
                routed_page = language_route.group(2)
                if routed_page not in outputs:
                    errors.append(f"{output}: broken language route {href}")
                elif language_route.group(3) and language_route.group(3) not in output_ids[routed_page]:
                    errors.append(f"{output}: {href} points to a missing anchor")
                continue
            file_part, _, anchor = href.partition("#")
            if not file_part:
                if anchor and anchor not in output_ids[output]:
                    errors.append(f"{output}: missing local anchor #{anchor}")
                continue
            if file_part.endswith(".md"):
                continue
            # Generated fragments are loaded into docs/D-home.html, so links
            # must resolve correctly from the fragment's own subdirectory.
            source_dir = posixpath.dirname(output)
            resolved = posixpath.normpath(posixpath.join(source_dir, file_part))
            if resolved not in outputs:
                errors.append(f"{output}: broken internal link {href}")
            elif anchor and anchor not in output_ids[resolved]:
                errors.append(f"{output}: {href} points to a missing anchor")
    return errors


def run(
    check: bool,
    site_dir: Path = DEFAULT_SITE,
    spec_dir: Path = SPEC_DIR,
) -> int:
    global OUTPUT, TREE, MANIFEST
    spec_dir = spec_dir.resolve()
    OUTPUT = site_dir / "docs"
    TREE = site_dir / "json" / "tree.json"
    MANIFEST = site_dir / "json" / "spec-manifest.json"
    local_pages = [parse_front_matter(path) for path in sorted(PAGES.rglob("*.md"))]
    spec_pages, hashes = imported_pages(spec_dir)
    files, tree = expected_files(local_pages + spec_pages, hashes, spec_dir)
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
            print(
                "Documentation output is stale. Run: "
                f"python3 tools/build_docs.py --site-dir {site_dir}",
                file=sys.stderr,
            )
            for path in stale:
                print(f"  stale: {path.relative_to(site_dir)}", file=sys.stderr)
            for path in orphaned:
                print(f"  orphaned: {path.relative_to(site_dir)}", file=sys.stderr)
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
    parser.add_argument(
        "--site-dir",
        type=Path,
        default=DEFAULT_SITE,
        help="site build directory containing docs/ and json/",
    )
    parser.add_argument(
        "--spec-dir",
        type=Path,
        default=SPEC_DIR,
        help="directory containing the canonical Zith specification Markdown",
    )
    args = parser.parse_args()
    try:
        return run(args.check, args.site_dir.resolve(), args.spec_dir.resolve())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
