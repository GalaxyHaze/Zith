---
id: guide-concurrency
title: Concurrency
section: Language Guide
output: zith--/guide/D-concurrency.html
aliases: language/D-concurrency.html
kind: editorial
---
# Concurrency

Zith-- does not provide threads, async execution, or a concurrency runtime in
HIR. The Zith reference describes a separate concurrency and runtime design;
it does not define current Zith-- behavior.

`async fn` is legacy reserved syntax. The parser accepts the declaration and
skips its body, but there is no async lowering or HIR contract behind it.
`yield`, `spawn`, and `await` are reserved tokens, not working statements.

Do not use these tokens as a preview of the current runtime. Keep concurrent
code at an application or library boundary until the runtime contract is
defined.

Use the [Zith concurrency reference](doc:reference-10-concurrency) to study
that design and [Implementation Status](doc:reference-implementation-status)
for the current Zith-- compiler status.
