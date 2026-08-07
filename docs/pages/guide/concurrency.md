---
id: guide-concurrency
title: Concurrency
section: Language Guide
output: guide/D-concurrency.html
aliases: language/D-concurrency.html
kind: editorial
---
# Concurrency

There is no async or thread model in HIR today, and concurrency is not planned as a function kind. The direction is `stdlib` and runtime APIs instead.

`async fn` is legacy reserved syntax: the parser accepts the declaration and skips its body entirely, with no async lowering and no HIR contract behind it. `yield`, `spawn`, and `await` are reserved tokens, not core operators or statements. Treat all four as historical parser affordances rather than a preview of the concurrency design.

Use the [Concurrency reference](doc:reference-10-concurrency) to understand the intended model and [Implementation Status](doc:reference-implementation-status) to track delivery.
