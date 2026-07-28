## 18. C Interop

Zith supports manual `extern fn` bindings on every target. Native builds which find libclang also
support a restricted, automatic C-header import path.

### 18.1 Automatic Binding via `.h`

Native builds with libclang can import a `.h` file. The importer exposes supported external,
non-variadic C functions directly; it does not generate Zith source.

```zith
import "mylib.h";

// Supported functions from mylib.h are available by their C linkage name.
my_function();
```

Only C ABI headers are accepted. `.hpp` files report that C++ headers are unsupported. Macros,
callbacks/function pointers, variadic functions, globals, bitfields, packed or anonymous records,
flexible arrays, and other non-representable layouts are not imported. Use manual `extern fn` for
APIs outside this surface and for all builds without libclang, including WASM and cross builds.

| C type | Imported Zith type |
|---|---|
| `void`, `_Bool`, integers, floats | ABI-width primitive equivalent |
| `T*`, `const T*` | Pointer preserving pointee constness |
| simple records and enums | Named foreign type |

The project may configure C parsing and linking in `ZithProject.toml`:

```toml
[ffi]
include_dirs = ["vendor/include"]
library_dirs = ["vendor/lib"]
libraries = ["mylib"]
defines = ["MYLIB_FEATURE=1"]
```

`-I`, `-D`, `-L`, and `-l` add command-line values after project values. Library names are
validated and linked without passing a shell command string.

### 18.2 Manual Binding with Semantic Annotation

Override or supplement auto-generated bindings to attach Zith-specific semantics:

```zith
// Equivalent declarations — malloc is a C function (no namespace)
// bindToC is subject to Zith namespace rules
fn bindToC = extern 'C' malloc(size: u64): unique opaque;
extern 'C' malloc(size: u64): unique opaque;   // same thing, no namespace alias
```

### 18.3 External (No Header)

For assembly routines, headerless libraries, or code deliberately outside the project:

```zith
// The linker resolves this; the compiler has no information about the function
fn bindTo = extern someAsmRoutine(x: u64): u64;
```

### 18.4 Exposing Zith to C

```zith
extern 'C' fn my_function(x: i32): i32 {
    x * 2
}
// Generates a C-compatible symbol, callable from C as an ordinary function
```

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
