# Zith Website Guide

How the site is built, and how to add documentation pages.

## Layout

```
partials/                 header.html, footer.html, head.html  <- single source for the chrome
tools/build.py            expands the includes, writes _site/
tools/build_docs.py       regenerates the docs fragments from docs/pages/*.md
docs/pages/*.md           Markdown source for the documentation
docs/D-home.html          documentation shell (SPA loader)
docs/                     generated fragments and shell live here after build
json/tree.json            generated navigation tree (sidebar structure)
css/                      tokens + per-area styles
js/                       per-page scripts
```

Every top-level page (`index.html`, `changelog.html`, `about/`, `playground/`,
`helios/`, `docs/D-home.html`) is a **source** that declares the
shared chrome with an include:

```html
<!-- include:head -->
<!-- include:header active="docs" -->
...
<!-- include:footer -->
```

`head.html` holds the `<head>` metadata shared by every page (the
`theme-color`). `tools/build.py` expands those includes into `_site/`, resolves
`{{baseurl}}`, and also injects the chrome into the Dev Log posts under
`about/posts/`.

## Preview

The site is static but the sources (the repo root) contain includes that are
**not yet expanded**. Never serve the root: it will render pages without the
shared header/footer. Build first and serve the output:

```bash
make serve          # build + serve _site/ on http://localhost:5501
```

Or the two steps separately:

```bash
make build                       # python3 tools/build.py
python3 -m http.server 5501 --directory _site
```

The Live Server extension is pinned to `_site/` via `.vscode/settings.json`
(`liveServer.settings.root`), so opening the folder and pressing *Go Live*
serves the built site. If you edit a source page, rebuild (`make build`).

On GitHub Pages the workflow (`.github/workflows/build.yml`) runs the build and
publishes `_site/`.

## Documentation

Each `D-*.html` file under `docs/` is a **content fragment** — the
inner HTML of a page, with no `<html>`, `<head>`, or `<body>`. The shell
(`docs/D-home.html`) loads them with `fetch()` when you click a sidebar link.

The build creates three entry routes from that shared shell: `/docs/` is the
neutral overview of Zith and Zith--, `/docs/zith/` opens the Zith language
design documentation, and `/docs/zith--/` opens the language and compiler
documentation for Zith--.
The previous `/docs/D-home.html` URL remains available.

### Write a page

1. Write the Markdown under `docs/pages/` (frontmatter with `id` and `title`).
2. Regenerate the fragments and the navigation tree:

   ```bash
   make docs          # python3 tools/build_docs.py
   make docs-check    # fails if the fragments are out of date
   ```

Supported Markdown: headings, bold/italic, inline code, fenced code blocks,
links (`.md` links are remapped to `./D-*.html`), blockquotes, lists, tables,
`---`, and callouts (`:::info`, `:::warning`, `:::tip`, `:::caution`,
`:::danger`).

Callout and content styles live in `css/docs.css`.

Run `node --test tests/docs-routing.test.js` to verify the route selection and
the generated static documentation output.
