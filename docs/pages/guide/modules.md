---
id: guide-modules
title: Modules
section: Language Guide
output: guide/D-modules.html
aliases: language/D-modules.html
kind: editorial
---
# Modules

Use `import` to bring in a namespace, `from` to inject visible symbols, and `export` to re-export a dependency. These forms are working compiler features.

```zith
import std/io/console as console;
@console.println("hello");

from std/io/console;
@println("hello");
```

`alias` names an existing symbol or namespace; `type` creates a distinct type. `use` is reserved for words, contexts, and operators and is currently blocked by semantic error E2010. Read the [Module System reference](doc:reference-02-module-system).
