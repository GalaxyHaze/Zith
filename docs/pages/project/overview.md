---
id: project-overview
title: Project
section: Project
output: project/D-overview.html
aliases: D-project-overview.html, D-roadmap.html
kind: editorial
---
# Project

Zith and Zith-- are sibling languages with separate contracts. Zith has a
language-design specification; the `zithc` compiler implements Zith--.
[Implementation Status](doc:reference-implementation-status) tracks Zith--
compiler behavior only and makes no claim about a Zith implementation.

## Contributing to documentation

Edit the Markdown files under `docs/pages` for editorial pages. The Language Reference is imported from `../Zith/docs`, including `Zith-spec.md`, `Zith--.md`, and `impl-status.md`; update that checkout for specification and implementation-status changes.

```bash
make docs
make docs-check
```

The generated HTML is committed because the public site is static.
