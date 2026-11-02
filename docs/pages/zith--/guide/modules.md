---
id: guide-modules
title: Modules
section: Language Guide
output: zith--/guide/D-modules.html
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

`alias` names an existing symbol or namespace and `type` creates a distinct
type; both work. `pub`, `mod`, `mod(N)`, and `mod(..)` visibility are
implemented for declarations and struct fields. `mod(..)` is unlimited;
`mod(N)` allows access up to N directory levels below the owner.

`use` belongs to Zith's contexts, words, and operators design. In Zith-- it
parses as a declaration but its body is skipped with no semantics, so it has
no effect. Read the [Module System reference](doc:reference-02-module-system)
for the Zith design and [Implementation Status](doc:reference-implementation-status)
for Zith-- behavior.
