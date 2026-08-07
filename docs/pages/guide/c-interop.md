---
id: guide-c-interop
title: C Interop
section: Language Guide
output: guide/D-c-interop.html
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
    printf("v=%d\n", 42);
    0
}
```

That program prints exactly `v=42`. Variadic functions, array-decayed parameters, `va_list` parameters, and function-pointer parameters all import. A declaration the importer cannot represent is skipped individually and recorded, so one unsupported function does not fail the whole header.

## What does not import

Preprocessor macros, global variables, bitfields, packed and anonymous records, and flexible array members are not imported. Struct-by-value parameters and results import as named foreign types, but the ABI is unverified and there is no Zith-visible layout, so passing records to C is not yet supported.

Every pointer coming from C is `?*T`. Check it with `is null` before dereferencing; the compiler currently accepts an unchecked `?*T` where `*T` is expected only because flow-sensitive narrowing has not landed.

Keep declarations small and match the exact C ABI. See [Raw & Unsafe](doc:guide-raw-unsafe), the [C Interop reference](doc:reference-18-c-interop), and [Implementation Status](doc:reference-implementation-status).
