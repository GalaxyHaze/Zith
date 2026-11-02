---
id: zith-subset-reference
title: Zith-- Reference
section: Zith--
output: zith--/D-reference.html
aliases: language/D-zith-subset-reference.html
kind: editorial
---
# Zith-- Reference

This page documents the Zith-- language compiled by the current `main`
compiler.
The [Implementation Status](doc:reference-implementation-status) page is
authoritative when a feature is partial.

## Bindings and parameters

Bindings use `let`, `var`, and `const`. A `let` binding is immutable and
cannot be reassigned. A `var` binding is mutable and rebindable. A `const`
binding has static storage when declared at module scope and always requires
an initializer.

```zith
fn main(): i32 {
    let a: i32 = 1;
    var b: i32 = 2;
    const LOCAL: i32 = 4;
    b = 3;
    return a + b + LOCAL;
}
```

Immutability propagates through paths. Assigning to a field, arrow member, or
index of a `let` or `const` binding is rejected. A `var` binding allows nested
writes.

Function parameters are immutable by default. Use `var p: T` to allow mutation
inside the function body.

Parameters may declare a default value after the type: `right: i32 = 5`. The
default applies when the call site omits that argument and all following
arguments. A parameter without a default cannot follow one with a default.

```zith
fn add(left: i32, right: i32 = 5): i32 {
    left + right
}

fn main(): i32 {
    add(7) + add(1, 2)
}
```

`extern var` and `extern const` declare externally defined storage. Both
require a type and cannot have an initializer. Use `= extern CSymbol` when the
Zith name differs from the linker symbol.

```zith
extern var counter: i32;
extern const rate: f64;
extern var local_name: i32 = extern c_counter;
```

## Functions and function values

Named function references and no-capture function literals are function values
with the C function-pointer calling convention. A function literal can be
written where an expression is expected:

```zith
fn main(): i32 {
    let double: fn(i32): i32 = fn (value: i32): i32 { value * 2 };
    double(21) - 42
}
```

Function literals cannot capture locals from an enclosing function. Closures
with captures are outside the Zith-- contract.

## Control flow

Conditions are parenthesized. Boolean negation is `not`; `not (cond)` is the
idiomatic form. `if` accepts an optional `else`. The chained form writes
`else (cond) { ... }`; the legacy `else if` form still compiles but emits
`W1008 DeprecatedSyntax`.

`while` compiles and emits `W1008 DeprecatedSyntax`. Prefer `for (cond) { ... }`.

`match` is a synonym for `when` and accepts the same syntax.

`when` arms use parenthesized patterns followed by the body, separated by
commas. The wildcard arm `(_)` must be last. A pattern operand that is not a
boolean desugars to equality with the subject.

```zith
when (status) {
    (Status.Ok or Status.Error) 10,
    (Status.Queued) and (retries < 5) 20,
    (_) 0
}
```

`in` tests whether a value is contained in a right-hand side. A range literal
is a built-in right-hand side.

```zith
if (x in 0.0>..<1.0) { ... }
```

Boolean operators have fixed precedence: `a or b and c` parses as `a or (b and c)`,
and `a and b or c` as `(a and b) or c`. `xor` accepts `bool` or two equal-width
integers.

`for` supports labeled loops: `outer: for ...`. `break outer;` and `continue
outer;` target the labeled loop. Unlabeled `break;` and `continue;` apply to
the innermost loop. `defer expr;` and `defer { ... }` run cleanup in reverse
registration order on fallthrough, `return`, `break`, `continue`, and `jump`.

`dock State(args)` is an expression that calls a state machine and returns its
final value. `jump Next(args);` ends the current state and tail-calls the next
one.

## Types and layout

Struct fields are private by default. `pub name: T = default` opens a field to
other modules. `mod name: T = default` and `mod(N) name: T = default` use the
existing module-visibility rules.

```zith
struct Box {
    data: i32,
    pub open: i32,
    mod sibling: i32,
    mod(2) deep: i32,
}
```

A local binding without an initializer may be declared before its first write.
Reading it before that first write reports `E3001`. The explicit opt-out is
`raw <name>`.

Optional values use the `?T` type and can contain `null` or a `T` payload. An
early-return guard such as `if (value is null) { return ...; }` narrows a local
optional pointer or aggregate for subsequent use. The flow proof is local and
does not extend through derived expressions or across function boundaries.

`component` defines a copyable POD type. Fields are primitives, arrays or
slices of those, or other components. Components cannot implement traits and
inline functions are restricted to pure transformations.

```zith
component Vec2 {
    [x, y]: f32,
    fn sum(self): f32 { self.x + self.y }
}
```

`type Name = T` creates a distinct nominal wrapper. `alias Name = T` is
transparent. Both require explicit casts for construction and extraction.

`raw fn` marks a function as an unchecked boundary. The body may use `raw`
operations and the compiler does not apply normal restrictions inside it.

### Layout intrinsics

`@sizeOf(T)` returns the size of any complete type as `u64`. `@offsetOf(S,
field)` applies to structs, while `@alignOf(T)` accepts any complete type;
both return `i32`. `@lengthOf(x)` and
`@ptrOf(x)` accept slices, arrays, string literals, and `lend`/`view` slice
parameters. For borrowed slices, `@ptrOf` returns the element pointer, not the
descriptor address. `@canonicalType(T)` returns a stable `u128` for the type.

### Explicit alignment

`#[alignas(N)]` reinforces the alignment of a struct or a `let`/`var`/`const`
binding. `N` must be a positive constant power of two supported by the target.

```zith
#[alignas(64)] struct TokenBlock { tag: u8 }
```

### Strings and characters

`char` is a UTF-8 code unit. A string literal has type `*char` and adapts
implicitly to `[]char`. There is no built-in `string` type in Zith--; use
`[]char` or `*char`. String literals concatenate with `+`, which produces an
allocator-origin value.

Escape sequences follow the C-style set (`\n`, `\r`, `\t`, `\\`, `\'`, `\"`,
`\xHH`, octal `\NNN`). `\#` produces a literal `#`.

## Ownership

`lend` and `view` are the ownership annotations implemented in Zith--. A
`lend T` or `view T` parameter receives the value by reference. Writing through
a `view` parameter reports `E4004`.

A `default` binding passed to a `lend` or `view` parameter requires `lend x`
or `view x` at the call site. Omitting the annotation or using the wrong mode
reports `E4005 OwnershipCoercionRequired`. The same binding cannot be passed
twice as `lend`, nor as both `lend` and `view`, within the same call.

```zith
struct P { x: i32 }

fn bump(p: lend P): i32 { p.x = p.x + 1; p.x }
fn read(p: view P): i32 { p.x }

fn main(): i32 {
    var q = P { x: 41 };
    _ = bump(lend q);
    _ = read(view q);
    q.x
}
```

Literals, temporaries, and call results do not require an annotation. A
binding already qualified as `lend` or `view` may also be passed without a new
annotation.

`unique`, `share`, `belong`, and `mut` in call position are rejected with
`E2007`/`E4007`. Zith's memory model uses `&T`, `&mut T`, `^T`, and `%T`;
those spellings are not part of Zith--.

### Opaque tagged values

`raw opaque` is an untagged `void*` for C interop. Zith-- also supports bare
`opaque`: a tagged open union stored as `{ *void, u32 }`. `T as opaque`
eradicates an addressable value into a local slot. `opaque is T` checks the
type id. `opaque as T` returns `?T`, producing `null` when the type id does not
match. `raw opaque as T` reinterprets without verification.

## Macros

Normal macros and `raw macro` are the only macro forms in Zith--. `tag` is a
Zith language item; the legacy `tag macro` spelling is rejected.

```zith
macro add(a, b) { a + b }
raw macro dbg(x) { @println(x) }
```

## Platform-specific imports

The resolver checks variants derived from the active target triple, using
canonical LLVM triple names such as `x86_64`, `aarch64`, `linux`, `darwin`,
and `windows`:

```text
foo.<arch>.<os>.zith
foo.<arch>.zith
foo.<os>.zith
foo.zith
```

A directory `foo/` is used as a module only when no matching file variant
exists. `foo.zith` takes precedence over a same-named directory.

## Implement blocks on primitives and common types

`implement` blocks may own primitives, `?T`, `[]T`, and `*char` in addition to
named types. Methods on these owners use the same implicit `self` receiver and
can be called with normal `.method()` syntax.

```zith
trait Value { fn value(self): i32; }

implement i32 as Value { fn value(self): i32 { 7 } }
implement ?char as Present {}
implement []char as SliceLen { fn len(self): i32 { 3 } }
```

Pointer and fixed-array owners (`*T`, `[N]T`) are rejected. Interface fields
are readable on concrete types and generic bounds, but not through a `dyn
Interface` value.

## Qualified method calls

A method call can be qualified with a trait or interface satisfied by the
receiver: `p.Trait.method()` or `p.Interface.method()`. The qualification
resolves only the method visible on that trait or interface, avoiding ambiguity
`E2008` when two conforming traits expose the same name.

```zith
trait A { fn pick(self): i32 { 1 } }
trait B { fn pick(self): i32 { 2 } }

struct P { x: i32 }
implement P as A {}
implement P as B {}

fn main(): i32 {
    let p = P { x: 0 };
    p.A.pick() + p.B.pick()
}
```

## Variadic slices

The last parameter of a function or method may be a variadic slice `[...]T`.
At the call site, all trailing homogeneous arguments are collected into a
temporary slice. An explicit `[]T` or `[N]T` may also be passed as the final
argument.

```zith
fn sum(rest: [...]i32): i32 { ... }

fn main(): i32 {
    sum();                  // empty tail
    let values: []i32 = [1, 2, 3];
    sum(values);            // explicit slice
    sum(1, 2, 3);           // auto-collected tail
}
```

## Diagnostics

Relevant diagnostics include:

- `E2007` — no compatible overload signature.
- `E2008` — ambiguous call.
- `E2024` — interface not satisfied.
- `E2027` — duplicate implementation.
- `E3005` — unproven nullable dereference.
- `E3009` — trait bound not satisfied.
- `E4004` — write through `view`.
- `E4005` — ownership coercion required at call site.
- `E4007` — invalid call ownership annotation.
- `E4008` — pointer escapes local scope.
- `E5001` — invalid IR.
- `E5002` — unreachable code.

## Removed restrictions

| Syntax or feature | Reason | Behavior |
|---|---|---|
| `global name: T = value` | globals use `const` | `E2010` with `const` suggestion |
| `mut` qualifier | bindings use `var` | `E2010` |
| `const fn` | ordinary functions fit the Zith-- contract | `E2010` |
| `unique` / `share` / `belong` | ownership outside the Zith-- contract | `E2010` |
| `tag` / `tag macro` | `tag` belongs to Zith; Zith-- accepts only normal/raw macros | `E2010` |
| Assignment to `let` / `const` | immutability | `E2010` |
| Field / arrow / index write through `let` / `const` root | immutability propagates | `E2010` |
| Assignment to `const` field | const storage | `E2010` |
| `const` without initializer | constant needs a value | `E2010` |
| Non-constant enum discriminant | variant needs an integer constant | `E3001` |
| Prefix `!` | negation uses `not`; `!` is reserved for postfix | parse error |
| `break label;` / `continue label;` with no active label | missing or ambiguous target | `E2010` |
| `++` / `--` | use explicit assignment | `E2010` |

## Outside the Zith-- contract

Zith-- does not include comptime evaluation, the NRA ownership model,
contexts, words, tag items, capturing closures, the `unsafe` hierarchy, or
the `fail` / `with` / `catch` / `throw` error workflow. These features are
described by Zith's separate language design; their presence there does not
mean they are planned for Zith--.

New ownership checks, SRA heuristics, `Result`-style types, feature flags, and
separate safety modes are not planned for the current Zith-- contract. The
`main` compiler builds Zith-- as its default language.

## Working language surface

Zith-- currently includes:

- `let`, `var`, and `const` bindings.
- structs, enums, unions, arrays, slices, packs, pointers, and `?T` optionals.
- named function values and no-capture function literals.
- `if`, `for`, `when`, `defer`, and `state` / `dock` / `jump`.
- modules, aliases, visibility, generic declarations, overloads, and variadic slices.
- traits, interfaces, `dyn` method dispatch, qualified method calls, and `implement` blocks on named types, primitives, `?T`, `[]T`, and `*char`.
- normal macros and `raw macro`.
- `extern fn`, C header imports, `raw opaque`, and bare `opaque`.
- `lend` and `view` ownership annotations.
- platform-specific module resolution and `#[alignas(N)]`.
- layout intrinsics: `@sizeOf`, `@offsetOf`, `@alignOf`, `@lengthOf`, `@ptrOf`, and `@canonicalType`.

For task-oriented explanations, continue with the [Zith-- Language
Guide](doc:guide-overview). This reference is the language contract; the
[Implementation Status](doc:reference-implementation-status) records the
compiler's current support.
