---
id: guide-c-interop
title: C Interop
section: Language Guide
output: guide/D-c-interop.html
aliases: language/D-c-interop.html
kind: editorial
---
# C Interop

The working core supports external function declarations and C header imports.

```zith
extern fn puts(message: raw opaque): i32;
```

C interoperability has safety boundaries. Keep declarations small, use the exact C ABI, and consult [Raw & Unsafe](doc:guide-raw-unsafe) plus the [C Interop reference](doc:reference-18-c-interop).
