---
id: guide-traits-interfaces
title: Traits & Interfaces
section: Language Guide
output: zith--/guide/D-traits-interfaces.html
aliases: language/D-traits.html
kind: editorial
---
# Traits & Interfaces

Traits and interfaces declare behaviour contracts. Implementation blocks attach
methods and trait implementations to a type.

```zith
trait Printable {
    fn print(self);
}
```

Use a trait when conformance is part of the type's contract:

```zith
struct Point {
    x: i32,
    y: i32,
}

implement Point as Printable {
    fn print(self) {
        // Formatting is omitted from this small example.
    }
}
```

Use an interface when a type only needs to expose a structural set of fields
and methods. Generic bounds can name either form:

```zith
fn show<T: Printable>(value: T) {
    value.print();
}
```

`dyn Trait` and `dyn Interface` support method dispatch through a runtime
vtable. Interface fields are available for conformance and generic bounds, but
they are not exposed through a `dyn Interface` value.

Zith-- supports declarations, conformance, generic bounds, and method
dispatch. Capability semantics and other features in the separate Zith design
are not part of the Zith-- contract. The [Traits & Interfaces
reference](doc:reference-04-traits-interfaces) describes that Zith design;
consult [Implementation Status](doc:reference-implementation-status) for
Zith-- compiler support.
