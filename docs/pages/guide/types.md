---
id: guide-types
title: Types
section: Language Guide
output: guide/D-types.html
aliases: language/D-types.html
kind: editorial
---
# Types

Zith has signed and unsigned integer types, floating-point types, `bool`, and `char`. Structs, components, enums, and unions are working core declarations.

```zith
struct Point {
    x: i32,
    y: i32,
}

enum Direction { North, South, East, West }
```

Start with explicit annotations when learning. The [Type System reference](doc:reference-03-type-system) defines arrays, slices, generic types, unions, and the experimental narrowing rules.
