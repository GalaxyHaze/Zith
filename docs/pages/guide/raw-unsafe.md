---
id: guide-raw-unsafe
title: Raw & Unsafe
section: Language Guide
output: guide/D-raw-unsafe.html
aliases: language/D-raw-unsafe.html
kind: editorial
---
# Raw & Unsafe

`raw` and `unsafe` express operations outside ordinary safety guarantees. The specification has stricter rules for raw contexts, pointer access, and the `Trust` capability.

Use raw code only at a small, audited FFI or platform boundary. The [Raw & Unsafe reference](doc:reference-13-raw-unsafe) is the canonical source; the planned ownership guarantees depend on NRA, which is not yet implemented.
