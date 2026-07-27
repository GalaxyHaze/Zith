---
id: guide-traits-interfaces
title: Traits & Interfaces
section: Language Guide
output: guide/D-traits-interfaces.html
aliases: language/D-traits.html
kind: editorial
---
# Traits & Interfaces

Traits and interfaces declare behavior contracts; implementation blocks attach methods and trait implementations to a type.

```zith
trait Printable {
    fn print(self);
}
```

Declarations parse and resolve, while advanced dispatch and capability semantics have implementation limits. Consult [Implementation Status](doc:reference-implementation-status) and the [Traits & Interfaces reference](doc:reference-04-traits-interfaces).
