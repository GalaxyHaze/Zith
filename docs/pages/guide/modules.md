---
id: guide-modules
title: Modules
section: Language Guide
output: guide/D-modules.html
aliases: language/D-modules.html
kind: editorial
---
# Modules

Use `import` to bring in a namespace, `from` to inject visible symbols, and `export` to re-export a dependency. These forms are working compiler features, resolved with correct paths.

```zith
import std/io/console as console;
@console.println("hello");

from std/io/console;
@println("hello");
```

`alias` names an existing symbol or namespace and `type` creates a distinct type; both work. `pub` and `mod` visibility work, while `mod(..)` and `mod(N)` are accepted by the parser with unverified sema behaviour.

`use` is reserved for words, contexts, and operators. It parses as a declaration but its body is skipped with no semantics, so it has no effect today. Read the [Module System reference](doc:reference-02-module-system).
