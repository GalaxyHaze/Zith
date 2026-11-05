#  Zith — Official Website

> This branch hosts the official website for the **Zith** programming language.

---

##  Creator

The site was created by [MieldewMeat](https://github.com/MieldewMeat).

---

##  Documentation

Documentation for the Zith programming language is currently **in progress** and will be published here as it grows.

### Documentation build

Editorial pages are Markdown-first under `docs/pages`. The Language Reference is synchronized from
the adjacent compiler checkout at `../Zith/docs`; generated HTML fragments remain versioned for
static hosting.

```bash
make docs
make docs-check
```

`make docs-check` fails if generated fragments, navigation, or the specification manifest are out
of date.

The docs reader also has an in-browser Markdown editor with a live preview.
Drafts are saved automatically in the current browser and do not change the
published site. Markdown sources are included in the static build, so editing
works without sign-in. Publishing edits is not enabled yet.

### Site build

The header and footer live in `partials/` and are expanded into `_site/` by
`tools/build.py` (no dependencies). The repo root holds the **sources** with
unexpanded includes, so build before serving — never serve the root, or the
pages render without the shared header/footer. Preview locally with:

```bash
make serve          # build + serve _site/ on http://localhost:5501
```

On GitHub Pages, `.github/workflows/build.yml` builds and publishes `_site/`.
See `GUIDE.md` for the page and documentation workflow.

Run the editor and routing checks with `node tests/docs-editor.test.js`
and `node tests/docs-routing.test.js`.

---

##  Contribute

We welcome community contributions! If you'd like to support the project by proposing documentation:

1. **Reach out** via [Discord](#https://discord.com/invite/a7h4cpWHg4) to coordinate with the team ( note: serve is not configured yet, We are sorry! )
2. **Follow** the project's contribution rules and guidelines
3. **Submit** your proposal for review

>  The team will evaluate all submissions before adding them to the site.

---

## ️ Tech Stack

- **Static HTML + CSS + vanilla JS** — no frameworks, no client-side runtime.
- **One small build step** (`tools/build.py`) expands the shared header/footer
  partials; the output is plain HTML served from `_site/`.
- **One theme** driven by CSS custom properties in `css/tokens.css`.

---

##  Notes

- This is a **live branch** — changes here reflect directly on the website
- Please make sure to follow the project rules before submitting any content

---

*Built with care for the Zith community.*
