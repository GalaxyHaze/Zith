---
id: guide-c-interop
title: C Interop
section: Language Guide
output: zith--/guide/D-c-interop.html
aliases: language/D-c-interop.html
kind: editorial
---
# C Interop

Declare a single C function by hand with `extern fn`, or import a whole header.

```zith
extern fn puts(message: *char): i32;
```

## Header imports

Header imports are driven by libclang and handle ordinary C headers.

```zith
import "stdio.h";

fn main(): i32 {
    _ = printf("v=%d\n", 42);
    0
}
```

That program prints exactly `v=42`. Variadic functions, array-decayed parameters, `va_list` parameters, and function-pointer parameters all import. A declaration the importer cannot represent is skipped individually and recorded, so one unsupported function does not fail the whole header.

## Header import limits

Object-like scalar macros and directly declared external scalar or pointer
globals are imported. `static` and transitively declared globals are skipped.
Bitfields, packed and anonymous records, and flexible array members are not
imported.

Passing C records by value is supported only for verified integer-class
layouts on 64-bit x86-64/AArch64 Linux (GNU or musl). Other target ABIs and
aggregate classes remain unsupported; consult [Implementation
Status](doc:reference-implementation-status) for the supported layouts.

Every pointer coming from C is `?*T`. An early-return `is null` guard narrows a
local pointer before use. Without proof, dereferencing or passing it where
`*T` is expected reports `E3005`; derived expressions and cross-function
proofs are not tracked.

Keep declarations small and match the exact C ABI. See [Raw & Unsafe](doc:guide-raw-unsafe)
for Zith-- usage, the [C Interop reference](doc:reference-18-c-interop) for
Zith's separate interop design, and [Implementation Status](doc:reference-implementation-status)
for compiler support.
