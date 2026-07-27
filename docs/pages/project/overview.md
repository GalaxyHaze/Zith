---
id: project-overview
title: Project
section: Project
output: project/D-overview.html
aliases: D-project-overview.html, D-roadmap.html
kind: editorial
---
# Project

Zith is developed alongside its compiler and specification. The language remains experimental, and implementation progress is tracked in [Implementation Status](doc:reference-implementation-status).

## Contributing to documentation

Edit the Markdown files under `docs/pages` for editorial pages. The Language Reference is imported from `../Zith/docs`; update that checkout for specification and implementation-status changes.

```bash
make docs
make docs-check
```

The generated HTML is committed because the public site is static.
