---
id: guide-errors
title: Errors
section: Language Guide
output: guide/D-errors.html
aliases: language/D-errors.html
kind: editorial
---
# Errors

The language specifies optional and result types (`?T`, `T!`) plus propagation and recovery syntax. Those operators are parsed but currently blocked by E2010 before code generation.

Use compiler diagnostics as they are today, and keep examples using the working core until error values are implemented. The planned rules are in the [Error Handling reference](doc:reference-08-error-handling).
