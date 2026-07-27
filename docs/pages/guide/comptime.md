---
id: guide-comptime
title: Comptime
section: Language Guide
output: guide/D-comptime.html
aliases: language/D-intrinsics.html
kind: editorial
---
# Comptime

Zith specifies `const` blocks, compile-time functions, reflection, and type transformation. The compiler parses relevant syntax but does not yet evaluate comptime blocks or `const fn` calls.

Layout intrinsics such as `@sizeOf`, `@alignOf`, and `@offsetOf` are working with an LLVM-enabled compiler. Other intrinsic calls currently emit W2009. See the [Comptime reference](doc:reference-11-comptime).
