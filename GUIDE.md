# Zith Documentation Guide

This guide explains how to write documentation for the [Zith programming language](https://zith-lang.org) website. The site uses a custom retro-OS-themed HTML documentation system (not Docusaurus anymore).

## How It Works

The documentation lives on the `gh-pages` branch in `html/documentation/`:

```
html/documentation/
  D-home.html          ← Full HTML page (shell with sidebar + content area)
  D-*.html             ← Content fragments loaded dynamically into the shell
json/tree.json         ← Navigation tree (defines sidebar structure)
css/documentation.css  ← Styling
js/loadTree.js         ← Loads tree.json + fetches content on click
```

Each `D-*.html` file is a **content fragment** — just the inner HTML of the page, no `<html>`, `<head>`, or `<body>` tags. The shell page (`D-home.html`) loads these fragments via `fetch()` when you click a sidebar link.

## Writing a New Page

### 1. Write in Markdown

Create a `.md` file following standard Markdown conventions. Example:

```markdown
---
id: my-page
title: My Page
---

# My Page Title

This is a paragraph with **bold** text and `inline code`.

## Subsection

- List item 1
- List item 2

See the [installation guide](./installation.md) for details.

> This is a blockquote.

| Name  | Type   |
|-------|--------|
| value | `i32`  |

```zith
let x: i32 = 42;
print(x);
```

:::info Note
This is an info callout box.
:::

:::warning Caution
This is a warning.
:::
```

### 2. Convert to HTML

Run the conversion script:

```bash
python3 tools/convert_md.py --file docs/my-page.md --output html/documentation/D-my-page.html
```

Or pipe:

```bash
python3 tools/convert_md.py < docs/my-page.md > html/documentation/D-my-page.html
```

### 3. Register in the Navigation Tree

Open `json/tree.json` and add your page. The tree is a nested JSON array:

```json
[
  {
    "title": "Home",
    "link": "../home.html",
    "children": [
      {
        "title": "Introduction",
        "link": "./D-intro-overview.html",
        "children": [
          { "title": "Installation", "link": "./D-intro-installation.html" }
        ]
      },
      {
        "title": "My Section",
        "link": "./D-my-section.html",
        "children": [
          { "title": "My Page", "link": "./D-my-page.html" }
        ]
      }
    ]
  }
]
```

Rules:
- `"title"` is what appears in the sidebar
- `"link"` is the path to the content fragment (relative to `html/documentation/`)
- The top-level `"Home"` link should have `"link": "../home.html"`
- Nest children inside `"children"` arrays
- "Home" is the only entry with `data-native="true"` (handled in JS)

## Markdown Reference

### What's Supported

| Markdown | HTML Output |
|---|---|
| `# Heading` | `<h1>Heading</h1>` |
| `## Heading` | `<h2>Heading</h2>` |
| `### Heading` | `<h3>Heading</h3>` |
| `**bold**` | `<strong>bold</strong>` |
| `*italic*` | `<em>italic</em>` |
| `` `code` `` | `<code>code</code>` |
| ```` ```lang ... ``` ```` | `<pre><code>...</code></pre>` |
| `[text](./page.md)` | `<a href="./D-page.html">text</a>` |
| `[text](https://... )` | `<a href="https://...">text</a>` |
| `> quote` | `<blockquote><p>quote</p></blockquote>` |
| `- item` (list) | `<ul><li>item</li></ul>` |
| `1. item` (ordered) | `<ol><li>item</li></ol>` |
| `\| A \| B \|` (table) | `<table><tr><th>A</th><th>B</th></tr></table>` |
| `---` | `<hr>` |
| `:::info ... :::` | `<div class="callout callout-info">...` |
| `:::warning ... :::` | `<div class="callout callout-warning">...` |
| `:::tip ... :::` | `<div class="callout callout-tip">...` |
| `:::caution ... :::` | `<div class="callout callout-caution">...` |

### Cross-References (Internal Links)

Always use relative paths ending in `.md`:

```markdown
[link text](./other-page.md)
```

The converter will automatically map `.md` paths to the correct `./D-*.html` paths. If the path isn't in the mapping, it will fall back to replacing `.md` with `.html`.

External links work normally:

```markdown
[Zith GitHub](https://github.com/GalaxyHaze/Zith)
```

### Callout Boxes

Callouts are fenced with `:::` markers:

```markdown
:::info Title Here
Body content here. Can span multiple paragraphs.
:::
```

Types: `info`, `warning`, `tip`, `caution`, `danger`.

### What NOT to Use

- **Mermaid diagrams** — Not supported. Write the diagram as a code block or remove it.
- **HTML directly** — Avoid raw HTML in Markdown; use the Markdown equivalents.
- **Frontmatter** — YAML frontmatter (`--- id: title: ---`) is automatically stripped; you can include it for your own reference but it won't appear in the output.

## File Naming Convention

Content fragments are named `D-{section}-{topic}.html`:

| Section | Example |
|---|---|
| Introduction | `D-intro-overview.html`, `D-intro-installation.html` |
| Language | `D-language-syntax.html`, `D-language-types.html` |
| CLI | `D-cli.html`, `D-cli-check.html`, `D-cli-build.html` |
| Advanced | `D-advanced-overview.html`, `D-advanced-unsafe.html` |
| FAQ | `D-faq-overview.html`, `D-faq-philosophy.html` |
| Community | `D-community-overview.html`, `D-community-chat.html` |

Keep the name short but descriptive. The `D-` prefix avoids conflicts with other files.

## Page Metadata

The `D-home.html` shell no longer displays per-page metadata. The header only contains the logo, the file bar, and the Helios CTA.

## Styling

The documentation uses:
- **Font:** `VT323` for main content, `"Fira Code", monospace` for sidebar
- **Colors:** Black background, white text, orange links, cyan ASCII art, yellow labels
- **CRT effects:** Scanlines, vignette, noise (configurable via settings gear)
- **Glow effects:** Text shadows on headings, links, and code
- **Callout boxes:** Colored left border (`--glow-orange` for warning, `--glow-cyan` for info, etc.)

Callout styles are defined in `css/documentation.css`. If you add a new callout type, add its styles there.

## Workflow Summary

1. Write `.md` file in `docs/` (or anywhere)
2. Run `python3 tools/convert-md.py -f docs/my-page.md -o html/documentation/D-my-page.html`
3. Add entry to `json/tree.json`
4. Open `html/home.html` in browser to test
5. Commit to `gh-pages` branch

## Conversion Script

The converter is at `tools/convert_md.py`. It handles:
- Frontmatter stripping
- All standard Markdown elements
- Callout boxes (`:::type:::)
- Internal link remapping (`.md` → `./D-*.html`)
- Mermaid diagram removal
- Code block syntax highlighting markers

If you add new pages, add their path mapping to the `PAGE_MAP` dictionary at the top of the script.
