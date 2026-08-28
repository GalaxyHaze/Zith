## 18. C Interop

> **Implementation status:** `extern fn` bindings are **working** on all targets. Native libclang
> C header import is **working** for common libc-style declarations (including variadic functions,
> array-decayed parameters, `va_list`, and function-pointer parameters). Object-like macros that
> expand to an exact scalar literal are also imported as constants. Function-like macros,
> strings, globals, bitfields, and complex layouts remain **unimported**. See
> [impl-status.md](impl-status.md).

Zith supports manual `extern fn` bindings on every target. Native builds which find libclang also
support a restricted, automatic C-header import path.

For Zith-style binders that need normal module, overload, and method semantics,
keep the full Zith signature on the left and put the C linker symbol on the right:

```zith
struct Window {}

pub fn createWindow(self: lend Sdl, title: *char, x: i32, y: i32,
                    width: i32, height: i32, flags: u32): ?*Window = extern SDL_CreateWindow;
pub fn destroy(self: lend Window): void = extern SDL_DestroyWindow;
```

Receiver binders must live in an `implement` block so method calls like
`window.destroy()` resolve through the owner type:

```zith
implement Window {
    pub fn destroy(self: lend Window): void = extern SDL_DestroyWindow;
}
```

The right-hand identifier must be a plain identifier. The compiler emits only an
external C symbol declaration; it does not create a Zith body. This lets a normal
`c/` module expose opaque SDL handles as Zith structs and attach thin methods to
them without relying on the C header importer or generating C shims.

### 18.1 Automatic Binding via `.h`

Native builds with libclang can import a `.h` file. The importer exposes supported external C
functions directly, preserving their variadic status; it does not generate Zith source.

```zith
import "mylib.h";

// Supported functions from mylib.h are available by their C linkage name.
my_function();
```

Only C ABI headers are accepted. `.hpp` files report that C++ headers are unsupported. Object-like
macros whose replacement is exactly one scalar literal are imported as constants; function-like
and string macros, globals, bitfields, packed or anonymous records, flexible arrays, and other
non-representable layouts are not imported. A single unsupported declaration or macro is skipped
rather than failing the whole header; the importer records the reason in `skippedFunctions` so the
rest of the file stays available. Use manual `extern fn` for APIs outside this surface and for all
builds without libclang, including WASM and cross builds.

### 18.1.1 Object-Like Macro Constants

An object-like macro defined in the imported header expands to a single scalar literal and becomes
a module constant. The value comes from the macro replacement token itself; there is no external C
evaluation. Zith imports `true`/`false` as `bool`, `'x'` as `char`, unsuffixed integers as `i32`,
unsuffixed floats as `f64`, and the known suffixes `i8`/`i16`/`i32`/`i64`/`u8`/`u16`/`u32`/`u64`
/`isize`/`usize` or `f`/`F` float suffixes when the value fits the target type.

```zith
import "constants.h";

fn main(): i32 {
    let answer: i32 = ANSWER;   // #define ANSWER 42
    let ratio: f64 = RATIO;     // #define RATIO 1.5
    return 0;
}
```

Colliding with any symbol already visible in the module is a duplicate declaration error. Macros
that expand to expressions, strings, or unsupported values are skipped, not imported.

### 18.1.2 Variadic C Functions

Variadic declarations use `...` as the final token of an `extern fn` parameter list. The fixed
parameters are type-checked normally; the variadic tail accepts any number of arguments and
reaches the native ABI as a variadic call.

```zith
extern fn printf(fmt: *char, ...): i32

fn main(): i32 {
    printf("n=%d\n", 7);
    return 0;
}
```

A non-`extern` function cannot declare `...`, and the tail must be the last element of the
parameter list. Declarations produced by the C header importer carry the same variadic flag into
resolution, HIR, and LLVM.

At a variadic call site the compiler applies the C default argument promotions to the variadic
tail: `f32` is widened to `f64`, and `bool`/`char`/small integer arguments are widened to `i32`.
Fixed parameters keep their declared ABI types.

String and character literals decode the C-like escape set (`\n`, `\r`, `\t`, `\0`, `\\`, `\'`,
`\"`, `\#`, and `\xHH`) before reaching LLVM, so `printf("v=%d\n", 42)` prints a real newline.
`\#` produces a literal `#` (useful as an escape hatch if string interpolation is added later).
Unknown escape sequences are rejected with `E0001` at the literal's span.

`char` literals such as `'B'` are typed as `char`, and `... as char` conversions are accepted.
Plain C `char` parameters and results import as Zith `char`.

| C type | Imported Zith type |
|---|---|
| `void`, `_Bool`, integers, floats | ABI-width primitive equivalent |
| `T*`, `const T*` | Nullable pointer `?*T`, preserving pointee constness |
| `T[N]`, `char[20]` (parameters) | Pointer to `T` / opaque pointer after C array decay |
| `va_list` | The decayed `struct __va_list_tag *` pointer carried by the function type |
| `int (*)(...)` (parameters) | Opaque pointer; callable through an existing C pointer value |
| simple records and enums | Named foreign type |

Because a C pointer imports as `?*T` (see
[8.1.1](08-error-handling.md#811-c-pointers-are-t)), reinterpreting one is written `as ?*T`;
`as *T` is rejected with `E3003` so the null case cannot be dropped silently. Passing a pointer
the other way needs no cast: any `*T` or `?*T` is accepted for a C `void*` parameter.

```zith
import "stdlib.h"

fn main(): i32 {
    let cell: ?*i32 = malloc(64) as ?*i32;
    free(cell);                             // ?*i32 -> void*, no cast
    var local: i32 = 7;
    free(&local);                           // *i32  -> void*, no cast
    return 0;
}
```

Records passed or returned **by value** import as named foreign types but have no verified ABI;
constructing or reading their fields from Zith is not supported yet (see Known Debt in
[impl-status.md](impl-status.md)).

The project may configure C parsing and linking in `ZithProject.toml`:

```toml
[ffi]
include_dirs = ["vendor/include"]
c_source_dirs = ["vendor/c", "src/native"]
library_dirs = ["vendor/lib"]
libraries = ["mylib"]
defines = ["MYLIB_FEATURE=1"]
```

`c_source_dirs` is scanned recursively for `*.c` files and compiles each one into
`cache/c-obj/<target>/...` before the native link step. Only roots declared there or through the
repeatable `--c-source-dir <DIR>` flag participate in C compilation; `-I` does not imply source
discovery.

`-I`, `-D`, `-L`, `-l`, and `--c-source-dir` add command-line values after project values.
Library names are validated and linked without passing a shell command string.

Native builds compile `*.c` through embedded `libtcc` when it is available. CMake first looks for
an existing `libtcc.h`/`libtcc.a`, including the `ZITH_TCC_ROOT` prefix. When none is found and
`ZITH_TCC_FETCH` is enabled, the configuration fetches and builds TinyCC 0.9.27 for the local host
as a static, PIC library. Disable fetching with `-DZITH_TCC_FETCH=OFF` to keep using the external C
compiler driver.

#### System C headers

System C headers resolve without any `-I` flag. On startup the compiler probes libclang once per
`(target triple, sysroot)` pair to discover the toolchain's default include directories, so
`import "stdint.h"` works out of the box. The clang resource directory (containing compiler-built
headers such as `stddef.h`) is found during CMake configuration from the LLVM package and pushed
both to the probe and to every header parse. Builds without libclang fall back to `/usr/include`
and `/usr/local/include` when they exist.

```zith
import "stdint.h";
```

These directories are searched **last**. Stdlib roots, `-I` roots, the workspace root, and the
importing file's own directory all take precedence, so a project-local `stdint.h` shadows the
system one. Pass `--no-system-includes` to disable the behaviour entirely; imports then resolve
only from explicitly configured roots. The discovered directories are part of the artifact cache
key, so changing them does not reuse stale cached modules.

Only C spellings with a `.h` extension resolve this way. Extensionless C++ names such as
`cstdint` are not supported.

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
