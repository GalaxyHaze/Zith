# ZITH Language Specification
**Draft v0.9 — 2026**

> Zith is a statically typed systems programming language with a small, composable core and a large toolbox for domain-specific work. It proves memory safety at compile time without a garbage collector or borrow checker.

## Introduction

Zith gives you full control with a minimal & clean syntax — you don't have to choose between verbose but safe or readable but slow. Its memory model, Node Resource Analysis (NRA), proves ownership and lifetime safety using five keywords: `lend`, `view`, `unique`, `share`, and `belong` — plus a `default` (no keyword) modifier.

Beyond memory safety, Zith has a general-purpose core with a much larger toolbox: state machines, contexts (DSLs), words (custom operators), comptime. You choose when to use them. Zith also follows the **Rule of Three**: "if a function needs more than three specialized tools, something went wrong."

This document is a draft of the language specification, currently v0.9.
Not every feature described here is implemented in the compiler.
For the exact picture of what works today, see [Implementation Status](impl-status.md). It serves three audiences: developers learning Zith for the first time, contributors working on the `zithc` compiler, and tooling authors building editors, linters, or other infrastructure around the language.

### Notation used in this document

| Symbol | Meaning |
|---|---|
| `?T` | Optional type — `T` or `null` ([§8.1](08-error-handling.md#81-failable-types)) |
| `T!` | Result type — `T` or an error ([§8.1](08-error-handling.md#81-failable-types)) |
| `?` / `!` (postfix) | Unwrap an optional / result, propagating or falling back ([§8.3](08-error-handling.md#83-propagation--fallback)) |
| `@name` | Compiler intrinsic or macro invocation ([§11.3](11-comptime.md#113-reflection), [§15](15-macros.md)) |
| `#name` | Variable or field attribute, e.g. `#thread_local` or `#volatile` |
| `::` | Scope resolution — reach past a shadowed name ([§2.3](02-module-system.md#23-namespace-access--scope-resolution)) |

This is a draft specification and remains subject to change as the compiler matures.

---

## Table of Contents

1. [Overview & Design Philosophy](#1-overview--design-philosophy)
2. [Module System](#2-module-system)
3. [Type System](#3-type-system)
4. [Traits, Interfaces & Capabilities](#4-traits-interfaces--capabilities)
5. [Functions](#5-functions)
6. [Mutability & Bindings](#6-mutability--bindings)
7. [Memory Model (NRA)](#7-memory-model-nra)
8. [Error Handling](#8-error-handling)
9. [Control Flow](#9-control-flow)
10. [Concurrency & Runtime APIs](#10-concurrency--runtime-apis)
11. [Comptime](#11-comptime)
12. [Assets](#12-assets)
13. [Raw & Unsafe](#13-raw--unsafe)
14. [Runtime: Polymorphism & Dynamic Behaviour](#14-runtime-polymorphism--dynamic-behaviour)
15. [Macros](#15-macros)
16. [Words](#16-words-custom-operators)
17. [Contexts](#17-contexts)
18. [C Interop](#18-c-interop)
19. [Project Configuration](#19-project-configuration)
20. [Standard Library](#20-standard-library)
21. [Best Practices & Patterns](#21-best-practices--patterns)
22. [Appendix — Keyword & Operator Reference](#22-appendix--keyword--operator-reference)

---

## 1. Overview & Design Philosophy

### 1.1 Who Zith Is For

If you are looking for just a 'new' language, clone or 'normal', so Zith is not for you. Zith was created for people starting to learn or open-minded, to discover new ways to think and structure your code, while having a powerful, readable, safe & expressive language.

### 1.2 Our Philosophy

Zith aims to be small and stable at its core — covering everyday needs — while offering a large kit that helps in specific domains where most languages need a lot of tricks to work.
The compiler is a copilot: it gives you the tools, and you build the systems.

| Everyday | Domain-specific |
|---|---|
| `struct`, `fn`, `lend`, `view`, `trait`, `interface` | `state`, `dock`, `jump` — for Games, State Machine, OS & embedded |
| `?T`, `T!`, `or` | `context`, `word` — for DSLs and APIs |
| `when`, `for`, `->` | runtime/stdlib concurrency APIs — for parallel work without special syntax |

### 1.3 Design Goals

- Expressive, minimal syntax that favors readability without sacrificing power.
- Memory safety via Node Resource Analysis (NRA).
- Composable behavior through traits, capabilities, and interfaces.
- Static, zero-overhead error handling with rich recovery semantics.
- Compile-time computation (`comptime`) as a first-class feature.
- Low-level control — state functions and musttail state transitions — without sacrificing safety in everyday code.
- Extensibility through words and macros, ideally scoped inside contexts rather than polluting the global namespace.

### 1.4 Context-Bound Extensibility (Best Practice)

Macros and words should ideally live inside a `context` block. Activating them globally is possible but discouraged — the same code smell as `using namespace std;` in C++.

```zith
// Preferred
use SQL {
    // SQL words and macros active only here
}

// Discouraged
use SQL;   // pollutes the rest of the file
```

### 1.5 Compilation Pipeline

The `zithc` compiler follows a multi-stage pipeline:

```
source -> lex -> scan -> resolve(import/symbols) -> sema -> comptime/solve -> NTA/NRA -> HIR -> LLVM
```
> Note: when you compile a library, after LLVM it outputs `.zirl` (Zith Intermediate Representation Library).

| Stage | Description |
|---|---|
| `source` | Receive arguments from CLI, load the file |
| `lex` | Tokenize the file into a `TokenStream` |
| `scan` | Find top-level declarations from the token stream |
| `resolve` | Resolve imported symbols, report duplicates |
| `sema` | Semantic analysis — name resolution, type checking, visibility |
| `comptime/solve` | Reserved for future generic instantiation, `comptime` evaluation, and the solved semantic view. Macro expansion currently happens during frontend parsing and uses the source AST directly |
| `NTA/NRA` | Accumulate semantic/resource facts, prove ownership rules, emit diagnostics, and apply only internal canonicalizations that do not change public ABI |
| `HIR` | Build High-level IR — the typed, desugared program with residual ownership facts attached when available |
| `LLVM` | Code generation via the LLVM backend |

`.zirl` files serve as cache and distribution format for compiled libraries — no headers needed, OS-agnostic, and you choose static or dynamic linking at the client side. Distribute once, link however the consumer prefers.

---

## 2. Module System

### 2.1 Import Keywords

| Keyword | Behavior |
|---|---|
| `import path` / `import path as name` | Imports a module under its path as a namespace, or under an explicit alias. Members are accessed as `name.symbol` |
| `from path` | Injects all visible symbols directly into scope, sugar for simple cases |
| `export path` | Re-exports a dependency; consumers of this module receive it as well |

```zith
import std/io/console as console;
@console.println("hi");

from std/io/console;
@println("hi");

export std/io/console;
```

### 2.2 `alias` vs `use` vs `type`

These three keywords are easy to conflate but serve distinct purposes:

| Keyword | Purpose | Example |
|---|---|---|
| `alias` | Create a name alias for a type, namespace, or symbol. | `alias Vec = std.collections.DynArray;` |
| `type` | Creates a new distinct type from an existing one | `type Celsius = f32;` |
| `use` | Bring a word, context, or operator into the current scope. | `use math.vec.dot as DOT;` / `use SQL;` |

```zith
alias Vec   = std.collections.DynArray;
alias print = std.io.console.println;

use math.vec.dot as DOT;
use SQL;

type angle = f32;
type celsius = f32;
type uuid = u128;
```

### 2.3 Namespace Access & Scope Resolution

Namespaces are accessed with `.` — e.g. `std.io.console.println`. The `::` operator reaches upward past a shadowed name to the outer scope:

```zith
let x = 10;
{
    let x = 20;
    @println(::x);   // 10, outer scope
}
```

### 2.4 Type Constraints vs. Union Separators

Type constraints and union variants look similar but use different separators to avoid ambiguity:

| Construct | Separator | Semantics |
|---|---|---|
| Type constraint | `or` (keyword) | Compile-time restriction / constraint |
| Union body | `,` (comma) | Runtime tagged union; variants separated by commas. |

```zith
// Type constraint -- compile-time dispatch
type Number = i32 or f64 or bool;
fn convert<T: Number>(val: T): string { ... }

// Union by default is runtime tagged
union AnyNumber { i32, f64, bool }
```

### 2.5 Visibility

| Modifier | Scope |
|---|---|
| *(none)* | Private — visible only within the declaring file. |
| `pub` | Public — visible to any importer. |
| `mod` | Module-local — visible to immediate sibling files in the same directory. |
| `mod(..)` | Visible to all sub-directories, unlimited depth. |
| `mod(N)` | Visible to exactly N levels of sub-directories deep. |

---

## 3. Type System

### 3.1 Primitive Types

| Category | Types |
|---|---|
| Unsigned integers | `u8`, `u16`, `u32`, `u64`, `u128` |
| Signed integers | `i8`, `i16`, `i32`, `i64`, `i128` |
| Floats | `f32`, `f64` |
| Other primitives | `bool`, `char`, `void` |
| Compiler-internal | `unknown` — a valid but unresolved type, not user-instantiable. `invalid` — a dead or uninitialized state (a moved variable, a proven-null variable). Neither can be named or stored by user code. |
| Special | `never`, `null` |
| Opaque | `opaque` — a reference type (`view` by default), equivalent to a tagged `void*`. `raw opaque` is an untagged `void*`, used for C interop. |

### 3.2 Slice & Array Types

| Syntax | Meaning |
|---|---|
| `[]T` | Slice — a fat pointer (pointer + length). String literals are `[]char`. |
| `[N]T` | Fixed-size array, stack-allocated. |
| `[_]T` | Deduced-size array — the compiler infers `N` from the initializer. |

#### Strings & Origin Tracking

`char` is a UTF-8 code unit. `string` is a built-in library type backed by `[]char` with UTF-8 encoding.

NRA tracks the **origin** of every string node — `literal`, `allocator`, or `local` (see [§7.1](07-memory-model.md#71-what-nra-tracks) for the complete set):

```zith
// []char implicitly casts to string, zero-cost (literal origin)
let s: string = "hello";

// Concatenation changes origin to allocator -- triggers allocation
let greeting = "hello" + " " + "world";
```

### 3.3 Enum

A closed set of named constants. All values must be known at compile time. Zith supports three styles:

#### C-style
```zith
enum Direction { North, South, East, West }
enum Status: i32 { Ok = 0, Err = 1, Pending = 2 }
```

#### Struct-backed
```zith
enum Color: rgb {
    Red   = { r: 255, g: 50,  b: 0,   a: 255 },
    Green = { 0, 255, 0, 255 },
}
```

### 3.4 Struct

#### Field Declaration & Grouping

Use individual fields for unrelated fields, and `[]` groups for semantically related fields that share a type:

```zith
struct Sample { name: string, age: i32 }

struct Point { [x, y, z]: f32 }

struct Transform {
    [x, y, z]:    f32,   // position
    [rx, ry, rz]: f32,   // rotation
}
```

#### Generic Structs & Self-Referential Patterns

```zith
struct Pair<T, U> { first: T, second: U }

// Doubly-linked list node
struct Node<T> {
    data: T,
    //Self = Node<T>
    next: ?unique Self,   // owns next; null at tail
    prev: ?belong Self,   // back-ref; lifetime tied to parent; null at head
}
```

#### Implementation Blocks

Structs, enums, and unions can declare methods without bodies in the type definition. The implementation goes in an `implement` block:

```zith
// Struct — declares methods, no body
struct Node<T> {
    data: T,
    next: ?unique Self,
    prev: ?belong Self,
    fn isHead(self): bool;   // declared, no body
    fn isTail(self): bool;   // declared, no body
}

// Implementation provides the bodies
implement Node<T> {
    fn isHead(self): bool { self.prev is null }
    fn isTail(self): bool { self.next is null }
}

implement Node<T> as Printable {
    fn print(self) { @println("Node({self.data})"); }
}

// Specific implementation for a concrete type
implement Node<f32> { ... }
```

Components define method bodies inline — they cannot use `implement`:

```zith
component Vec2 {
    [x, y]: f32,
    fn length(self): f32 { sqrt(self.x*self.x + self.y*self.y) }
    fn dot(self, other: Self): f32 {
        self.x*other.x + self.y*other.y
    }
}
```

> Structs, enums, and unions **declare** methods (no body) and **define** them in `implement`. Components **define** methods inline and cannot use `implement`.

#### `self`, `other`, `Self`

`self` is the current instance. `other` is a shorthand for a second instance parameter. `Self` (capitalized) refers to the concrete type currently being implemented.

### 3.5 Component

A plain-old-data (POD) struct, **copy by default** alongside primitives. Components cannot implement traits, and any inline functions are limited to pure transformations.

```zith
component Vec2 {
    [x, y]: f32,
    fn length(self): f32 { sqrt(self.x*self.x + self.y*self.y) }
    fn dot(self, other: Self): f32 { 
        self.x*other.x + self.y*other.y 
    }
}
```

A component must satisfy all of the following constraints:

- Every field is a primitive, another component, or an array/slice of either — no structs, unions, or non-integer enums.
- No trait declarations (`component Name: Traits` is not allowed).
- Inline functions are permitted but restricted to pure transformations:
  - No side effects (I/O, allocation, global mutation).
  - Only arithmetic, comparisons, and field access.
  - Must return a value — `void` is not allowed.
- Copying is always bitwise (memcpy-safe).
- Layout is C-compatible — no vtable, no fat pointers.
- No self-referential fields (`?unique Self`, `?belong Self`).

### 3.6 Union

By default, a `union` is runtime-tagged, with variants separated by commas:

```zith
union Value { i32, f64, bool }

enum Flag { A, B, C }
let flag = Flag.A;

// Type hint forces union deduction
let x: union = when (flag) {
    A = 42,
    B = 3.14,
    C = true,
};
```

> Without an explicit `union` type hint, the compiler cannot deduce a union — it must be stated explicitly.
>
> `dyn` works the same way as a type hint — see [§14.2](14-polymorphism.md#142-dyn-as-a-type-hint).

`raw union` is an untagged C-style union, valid only inside `raw` contexts. Accessing the wrong variant is undefined behavior.

#### ADT-style (Named Variants)

Unions can also have named variants, similar to Rust enums:

```zith
union Shape {
    Circle = { radius: f32 },
    Rect   = { w: f32, h: f32 },
    Point,
}

fn area(s: Shape): f32 {
    when (s) {
        Circle = s.radius * s.radius * 3.14f,
        Rect   = s.w * s.h,
        Point  = 0,
    }
}
```

You can also combine `enum` with `union` for compile-time constants that carry data:

```zith
enum Constants: union {
    pi      = 3.14f,
    vector  = |x: -1, y: 0, z: -1, w: 1|,
    nothing = 0,
}
```

> Both `union` & `enum` can have methods, but neither can implement traits.
>
> Pack literals in `enum:union` variants are treated as anonymous structs with a concrete layout.

### 3.7 Union Narrowing (`is`) & Flow Typing

The `is` operator narrows a union within a branch. Branches are isolated — moves inside one branch don't affect others. After the block completes, the type **widens back** to the full union. The underlying tag does not revert — the value stays `i32` internally — but the type system treats it as the full union again.

When you write a conditional as an expression and not every branch returns a value, the missing branches return `null`. The result becomes `?T`:

```zith
let result = if (v is i32) v;   // ?i32 — missing branch returns null
```

Using the narrowed value outside the `if` is a **compile error**. Recover it by storing the result of the branch expression.

```zith
fn handle(v: Val): void {
    if (v is i32) {
        @println("int: {v}");    // v is i32 here
    } else (v is f64) {
        @println("float: {v}");
    } else {
        @println("str: {v}");    // compiler knows v is []char here
    }
    // v is Val again (full union)
}

// Widening — mutation inside a branch
when (v) {
    i32 = { v = 42 },   // v is still i32 internally, but types as Val after
}
// v is Val again here

// when with branch tags
when (v) {
    n: i32  = @println("int: {n}"),
    f64     = @println("float: {v}"),
    _       = @println("other"),
}

when (shape) {
    Circle = @println("circle r={shape.radius}"),
    Rect   = let [val..] = shape,   // val = w; remaining fields ignored
    _      = @println("other"),
}

// Standalone boolean narrowing & compile-time reflection
// All boolean conditions must be wrapped in parentheses.
let numeric  = (v is i32) or (v is f64);
let isStruct = (T is @struct);
```

### 3.8 Generics

```zith
// Explicit constraints
fn serialize<T: Serializable + Printable, U: Clone>(val: T, ctx: U): string { ... }

// Implicit constraints inferred from usage at the call site
fn add(a, b) { a + b }   // Arithmetic is implicitly required
```

### 3.9 `when` — Pattern Matching

```zith
when (count) {
    0       = @println("none"),
    1       = @println("one"),
    2..=10  = @println("few"),
    _       = @println("many"),
}

// As an expression
let label = when (score) { 
    90..100 = "A",
    70..90 = "B", 
    _ = "C" 
};

// '..' ignores the remaining fields
when (point) {
    [x..] = @println("x=", x),
    _     = @println("no match"),
}

// '..' before a binding captures the last element
when (point) {
    [..w] = @println("w={w}"),
    _     = @println("no match"),
}
```

### 3.10 Cast Operator

```zith
let n: i32 = 42;
let f = n as f64;
```

---

## 4. Traits, Interfaces & Capabilities

### 4.1 Traits vs. Interfaces

| | Trait | Interface |
|---|---|---|
| **Typing** | Nominal — must be explicitly implemented. | Structural (duck-typed) — automatically satisfied when fields match. |
| **Extensible** | Yes — via `extends`, or as a precondition using `requires`. | No — interfaces cannot extend each other, though a trait may `requires` one. |
| **Has implementation?** | Yes — default method bodies are allowed. | No — declaration only. |
| **Field access** | Only through a trait that `requires` the interface. | Yes — directly, since interfaces are structural. |

### 4.2 Traits

A `requires Cond` clause goes **before** `trait` or `interface`. It forces any implementing type to also satisfy that condition. Traits may provide default method bodies. Use `self` / `other` as the conventional instance parameters, and `Self` (capitalized) for the concrete implementing type.

```zith
trait Printable {
    fn print(self);
    fn println(self) { self.print(); io.writeln(""); }
}

requires Printable
trait JsonSerializable {
    fn print(self);
    fn toJson(self): string;
}

// Disambiguate overlapping method names using the trait as a namespace
Printable.print(self);
JsonSerializable.print(self);
```

### 4.3 Interfaces

Interfaces are structural — if it quacks, it's a duck. Any type that has the required fields satisfies the interface automatically, without an explicit `implement` declaration. You can also add `requires` to interfaces.

```zith
// will only accept structs and reject components
requires @isStruct
interface iPositioned { [x, y, z]: f32 }

requires iPositioned
trait Movable {
    fn translate(self, dx: f32, dy: f32, dz: f32) {
        self.x += dx; self.y += dy; self.z += dz;
    }
}

// Any struct with x, y, z: f32 satisfies iPositioned automatically
struct Enemy { [x, y, z]: f32, health: i32 }
```

### 4.4 Capabilities — Built-in Reference

Capabilities are special traits that feed the compiler more information, unlocking special rules and optimizations.

| Capability | What it does |
|---|---|
| `Copy` | Implicit bitwise copy. Components and primitives are `Copy` by default. |
| `Functor` | `operator()` — makes a type callable like a function. |
| `Arithmetic` | Operators `+`, `-`, `*`, `/`, `%`, and so on. |
| `Error` | `operator throw`, required for `throw MyError;`. |
| `Null` | A negative capability — its traits activate only once NRA has proven a value IS `null`. Outside the proven-null branch, calling the method is a **compile error**. |
| `Fail` | A negative capability — its traits activate only once NRA has proven a value IS an error. Cannot coexist with `Null` on the **same level**, but `?T!` can have `Null` on the outer level and `Fail` on the inner. |
| `Allocator` | To provide custom allocators |
| `Generator` | Allows creating runtime-defined resumable or streaming protocols without introducing a dedicated core function kind. |
| `Share` | Required for `global: share` and crossing thread boundaries |
| `Lent` | Enables `global: unique`, a runtime-checked exclusive borrow. `global` bindings cannot be moved — `Lent` manages thread-safe distribution. Also allows `lend` parameters. |
| `Trust` | A trait extending `Trust` may contain `raw fn` methods callable from safe contexts. |
| `Unique` | Marks a singleton type. It cannot be instantiated — the type name itself acts as the instance. All fields must implement `Share` (thread-safe). A `unique Local` variant is a singleton thread-local. |

#### `Null` & `Fail` — Negative Capabilities

Dispatch is based on NRA state. Inside a proven-null branch, the `Null` trait unlocks. Outside it, calling `Null` methods is a **compile error**:

```zith
implement Config as Null {
    fn onMissing(self) { log("Config was null -- using defaults"); }
}
implement Config! as Fail {
    fn onError(self) { log("Config load failed"); }
}

let cfg: ?Config = loadConfig();
if (cfg is null) {
    cfg.onMissing();   // OK — Null trait unlocked here
}
// cfg.onMissing();   -- COMPILE ERROR: outside null branch

// Multi-level: ?T! — Null on outer, Fail on inner
// cfg.onError() is only valid inside a proven-error branch
```

`Null` and `Fail` are per-level. A `?T!` value can activate `Null` (outer `?`) independently from `Fail` (inner `!`).

#### `Trust` — Safe Sections with Raw Code

```zith
trait Place extends Trust {
    raw fn sample(): i32 {}
}

fn safeCaller(a: Place) {
    let v = a.sample();
}
```

#### `Unique` — Singleton Types

```zith
struct AppConfig: Unique { host: string, port: u16 }

AppConfig.host = "localhost";
AppConfig.port = 8080;
// let cfg = AppConfig { ... };  -- COMPILE ERROR
```

#### `Share` — Mutable State Across Thread Boundaries

```zith
// Share is mutable, multiple names, multiple threads, all can write
global counter: share Atomic<i32> = 0;

// Without the Share capability, cross-thread access is a compile error
struct LocalOnly { data: i32 }
// global bad: share LocalOnly = ...;  -- COMPILE ERROR: lacks Share
```

### 4.5 Operator Overloading

Operator overloading is capability-based and strict — you implement only the operators you need:

```zith
implement Vec3 as Arithmetic {
    fn +(self, other: Self): Self { ... }
    fn -(self, other: Self): Self { ... }
}

implement Pipeline as Functor {
    fn (self, input: []u8): []u8 { ... }
}
let out = pipe(raw_bytes);
```

### 4.6 Extension (`extends`)

`extends` copies the base type's fields and traits into the new struct. An optional `:` after the base lists further traits to implement:

```zith
struct Dog extends Animal {}
struct T extends Base: Transform, Collision {}

// Traits may also extend capabilities or other traits
trait SafeBuffer extends Trust {
    raw fn readByte(self, offset: u64): u8 {}
}
```

---

## 5. Functions

### 5.1 Return Types & Implicit Returns

```zith
fn add(a: i32, b: i32): i32 { a + b }   // explicit type, implicit return
fn add(a: i32, b: i32)      { a + b }   // inferred type

// Bounds-checked indexing: returns the element if in range, otherwise
// propagates null via the implicit optional from '?'.
fn first<T>(slice: []T): ?T {
    slice[0]?
}

fn pick(flag: bool): i32 {
    if (flag) {
        1
    } else {
        2
    }
}
```

> The compiler cannot infer a `union` or `dyn` return type without an explicit type hint.
> Non-void functions may use an implicit return only when every possible path produces a value
> or otherwise terminates. Falling off an `if` without `else`, a `when` without a default, or an
> empty body is a diagnostic.

### 5.2 Function Kinds

| Kind | Description |
|---|---|
| `fn` | Standard runtime function. |
| `const fn` | Compile-time function; parsing is in progress, evaluation is not implemented yet. |
| `state` | Direct state functions; `dock` starts a state machine and `jump target(args)` is a terminating LLVM `tailcc`/`musttail` transition. States in one machine share a return type but may have different parameter lists ([§9.4](09-control-flow.md#94-state-functions-and-state-machines)). |
| `raw fn` | Always unchecked, bypassing NRA and safety checks for C interop. |
| `extern fn` | Fixed C ABI linkage; never name-qualified and never overloaded. |

> The five function kinds are exclusive and cannot be combined: there is no `raw const fn`,
> `extern raw fn`, or similar spelling. `raw fn` and `extern fn` are separate concerns: `raw fn`
> opts out of NRA, while `extern fn` selects the C ABI.

Macro calls use the `@` prefix — `@println`, `@log`, `@serialize` — while ordinary function calls use a bare name, such as `console.write`, `process`, or `save`. See [§15](15-macros.md) for the full rule.

### 5.3 Runtime Tasks, Coroutines, and Concurrency APIs

```zith
// Runtime task types are ordinary library types.
// The core language has no `async fn`, `yield`, `spawn`, or `await` syntax.
fn fetch(url: string): Task<Response!> {
    return runtime.schedule(url);
}
```

Concurrency is modeled by `stdlib` or runtime APIs, not by dedicated syntax or a special function
kind. A library may expose `Task<T>`, `Generator<T>`, channels, executors, or thread handles, but
the compiler only sees ordinary declarations, calls, traits/capabilities, and the NRA facts needed
to validate resource usage around them.

## 6. Mutability & Bindings

### 6.1 Deep Mutability Model

Zith uses deep mutability: a modifier on a binding flows into every nested field. Fields inside a struct inherit the mutability of the instance that holds them — no per-field `mut` annotation needed.

### 6.2 Binding Keywords

| Keyword | Controls | Semantics |
|---|---|---|
| `let` | Binding | Immutable — cannot be reassigned. |
| `var` | Binding | Mutable — can be reassigned. |
| `global` | Binding | Static storage duration. |
| `const` | Binding | Compile-time constant. |


> `let`/`var` control reassignability of the binding itself. Content mutability is handled separately through memory modifiers ([§7](07-memory-model.md)).

```zith
// let/var control REBIND only. Content mutability comes from memory modifiers.
let x: mut Point;      // cannot reassign x; Point's fields are mutable (mut)
var y: Point;          // can reassign y; Point's fields are immutable (default, no mut)

// lend, unique, share, belong → imply mut
fn update(p: lend Point) { p.x += 1; }  // p is mutable (lend implies mut)
let r: unique Resource = acquire();     // r's fields are mutable (unique implies mut)

// view → implies immutable
fn read(c: view Config) { ... }         // c is read-only (view implies immutable)

const PI = 3.14159;
const COUNT: mut = 0;
COUNT += 1;   // valid at compile time only
```

### 6.3 Destructuring

```zith
let [x, y, z]: f32 = 1.0f;             // grouped same type, related semantics
let name: string; let age: i32; // individual unrelated
let [x,y,z] = | 5,4,'c'|;   // pack literal — see [§6.4](#64-pack-literals)

// If the loop never runs, 'or' supplies the fallback value
let r = for ([acc, i]: i32), (i in 0..n) {
            acc *= i + 1
        } or 0;
```

### 6.4 Pack Literals

Packs group heterogeneous values into a lightweight tuple-like structure. They are declared with `| |` and can be destructured with `[ ]`:

```zith
// Pack literal
let p = | 5, 4, 'c' |;

// Destructure
let [a, b, c] = p;

// Used in for loops with type annotation
let r = for ([acc, i]: i32), (i in 0..n) {
            acc *= i + 1
        } or 0;
```

> Packs are like anonymous structs — the compiler extracts fields by order and passes them as function arguments. They have a concrete layout determined at compile time. They are primarily used for destructuring and as loop accumulators.

---

## 7. Memory Model (NRA)

### 7.1 What NRA Tracks

The ownership system is split conceptually into two layers:

- `NTA` accumulates semantic facts over a representation that still preserves resource identity,
  qualifier distinctions, captures, escapes, branch facts, and return-path structure.
- `NRA` consumes those facts, applies the four ownership rules, emits diagnostics, and performs
  only the internal canonicalizations that are safe to materialize after the proof boundary.

NRA watches every value in your program and classifies it into one of three states:

| State | Meaning |
|---|---|
| `alive` | Ready to read or use |
| `dead` | Moved away — you cannot read it, only reassign |
| `lent` | Temporarily borrowed — exclusive while the borrow lasts |

It also tracks the **origin** of each node — where the value came from:

| Origin | Example |
|---|---|
| `literal` | `"hello"`, `42` — zero cost, no allocation |
| `allocator` | Heap-allocated via `new` or concatenation |
| `local` | Stack variable |
| `view` | Read-only reference to another node |

With these two axes (state + origin), NRA enforces the rules in [§7.4](#74-the-four-nra-rules).
NTA also records aliasing, branch-local facts, whether a return value is the same node received as
an argument, and whether a `belong` or borrowed value escapes its legal region.

### 7.2 Move Semantics

Moving `a` to `b` redirects the name `b` to `a`'s node. The name `a` is considered **dead** / **invalid** and cannot be read — only reassigned:

```zith
var a = Point { x: 1.0, y: 2.0 };
let b = a;                          // b -> a's node; a becomes dead
// println(a.x);                    -- COMPILE ERROR: a is dead
@println(b.x);                       // OK

a = Point { x: 3.0, y: 4.0 };      // OK: reassignment creates a new node for a
```

In effect, if `a` is never reassigned, it is as though `a` never existed and `b` has held `Point { x: 1.0, y: 2.0 }` all along.

### 7.3 Memory Modifiers

| Modifier | Relationship | Common use |
|---|---|---|
| `default` | Owned. Lifetime follows the binding. | Variables, struct fields |
| `lend` | Exclusive mutable temporary. Cannot be stored, moved, or captured — but **can be returned**, passing the promise to the caller. `belong` fields can also be passed as `lend`. | Passing mutable references to functions |
| `view` | Read-only, non-owning reference. Many views may coexist. | Inspecting without ownership |
| `unique` | Single-owner guarantee — only one name in the graph. | Ownership-transfer patterns |
| `share` | Multiple names, same node, statically validated — no ref-counting. Mutable. | Compile-time-proven sharing |
| `belong` | Part-of relationship. Node lifetime tied to its parent; cannot be stored independently. Can be passed as `lend`. | Back-pointers, hierarchies |

> `unique` provides compile-time single-owner guarantees for local bindings. In a `global` context, `unique` becomes runtime-checked — the compiler enforces exclusive access at program startup. `global` bindings cannot be moved; the `Lent` capability manages thread-safe distribution.

> In practice, most code only needs `lend` and `view`.

#### Implicit Mutability

Each memory modifier carries an implicit content mutability level:

| Modifier | Implies | Example |
|---|---|---|
| `lend` | Mutable | `fn update(p: lend Point) { p.x += 1; }` — `p` is mutable |
| `unique` | Mutable | `let r: unique Resource = ...;` — `r`'s fields are mutable |
| `share` | Mutable | `global counter: share i32 = 0;` — mutable across threads |
| `belong` | Mutable | `parent: ?belong Self` — mutable back-pointer |
| `view` | Immutable | `fn read(c: view Config) { ... }` — `c` is read-only |
| `default` | Depends on `mut` | `let x: Point;` — immutable. `let x: mut Point;` — mutable. |

`default` is the only modifier where mutability is explicitly controlled via the `mut` keyword. All others carry their mutability semantics implicitly.

### 7.4 The Four NRA Rules

**Rule 1 — Argument Exclusivity.** In any call expression, each argument must refer to a distinct node, without exception:
- Duplicating a `default` / `unique` / `lend` argument → **ownership error**.
- Duplicating a `share` / `view` argument → **logic error** (passing the same resource twice is almost certainly a bug).

**Rule 2 — No Dead Node Access.** A symbol cannot be read while its node is `dead`.

**Rule 3 — No Escaping `belong`.** A `belong` node cannot be stored anywhere whose lifetime exceeds any node in its dependency vector. At every use, all of its parents must be `alive`.

**Rule 4 — `lend` Behavioral Promise.** A `lend` value cannot be stored, moved, or captured. It may be passed as a call argument or returned — in the latter case, passing the promise on to the caller.

> For details on how NRA resolves nodes and validates these rules, see [§7.9](#79-how-nra-resolves-nodes).

### 7.5 NRA in Practice

```zith
// lend -- exclusive temporary borrow
fn scale(p: lend Point, factor: f32) { p.x *= factor; p.y *= factor; }
let pt = mut Point { x: 3.0, y: 4.0 };
scale(pt, 2.0);
@println(pt.x);   // OK: borrow ended

// view -- multiple read-only refs
let v1: view Point = pt;
let v2: view Point = pt;   // fine

// share -- no ref-count, statically proven
let a: share Config = load();
let b: share Config = a;   // both point to the same node

// belong -- back-pointer cannot outlive its parent
struct Tree<T> {
    data:     T,
    children: []unique Self,
    parent:   ?belong Self,
}

// belong fields can be passed as lend
fn getParent(self: view Node): lend Node { self.parent }
```

### 7.6 Boundary Before HIR

The main NRA proof runs before the final HIR is formed. That boundary exists so the analysis still
sees:

- binding identity and resource graphs;
- the difference between `default`, `view`, `lend`, `unique`, `share`, and `belong`;
- branch facts, narrowing facts, and return-path equivalence;
- call, capture, and escape structure before lowering erases it.

The final HIR is therefore not the place where ownership is re-proven. It receives a typed,
desugared, NRA-validated view of the program plus only the residual facts that still matter for
lowering, cache serialization, and backend hints.

### 7.7 Residual Facts and Internal Canonicalization

NRA may materialize limited internal canonicalizations after it has proven the ownership contract,
but those rewrites do not change a public signature or observable ABI. For example, forwarding a
proven move internally or removing a temporary introduced only to preserve ownership is valid;
redefining an exported function's calling convention is not.

Residual facts that may survive into HIR include:

- consumed vs. non-consumed value state when lowering depends on it;
- non-null or otherwise narrowed facts that affect control-flow lowering;
- borrow, capture, or escape decisions that codegen and caching must preserve;
- internal calling-convention details only when they stay behind a stable boundary.

LLVM is not the source of truth for ownership. At most it receives hints already decided by NRA,
such as `nonnull`, `noalias`, `readonly`, `nocapture`, or opportunities to remove redundant
temporaries and stores.

### 7.8 Self-Referential Types

```zith
struct Node<T> {
    data: T,
    next: ?unique Self,
    prev: ?belong Self,
}

implement Node<T> {
    fn append(self: lend Self, data: T) {
        self.next = unique Node { data, next: null, prev: belong self };
    }
}
```

- Freeing the head frees the entire chain, since `next` forms a `unique` ownership chain.
- NRA guarantees `prev` (`belong`) never outlives its owner.
- `belong` fields may be passed as `lend` to functions.

### 7.9 How NRA Resolves Nodes

> *This section is relevant for tooling authors and compiler contributors.*

Every symbol gets a **resource node** before final HIR lowering. NTA and NRA are lazy in the sense
that they validate nodes when use, view, move, return, capture, or escape facts make the proof
relevant.

#### Node Validation

When you access a node, NRA checks:

1. The node itself is `alive` (not `dead`).
2. Every node in its **dependency vector** — the fields or resources it belongs to or references — is also valid.

If a node is `dead` (say, after a move), NRA records where and why. You get an error pointing right at the violation.

#### Function Evaluation

NRA caches function results. If it has seen a function before, it reuses the cached analysis.
Otherwise, it inspects every return path:

- **Every** path returns one of the function's arguments → caller's node is **not consumed**
  (ownership stays with you).
- **Any** path doesn't return an argument → the result is **consumed**.

Those return facts are preserved into HIR only in residual form. HIR should not have to rediscover
which node a return came from.

#### Branch Isolation (`if` / `else` / `when`)

Each branch runs in isolation. A move inside one branch cannot affect the others. After all branches
complete, NRA applies the side effects of whichever branch actually ran and emits only the merged
facts that lowering still needs.

## 8. Error Handling

Error handling in Zith is fully static and return-based — there are no exceptions, and no semicolon is required after `?` or `!`.

### 8.1 Failable Types

| Syntax | Meaning | Propagated by |
|---|---|---|
| `?T` | Optional — `T` or `null`. | `?` (postfix) |
| `T!` | Result — `T` or an error. Equivalent to a `Result<T, E>` where `E` implements `Error`. The compiler infers an anonymous error union when multiple error types are possible. | `!` (postfix) |

Failable types may be stacked, and the notation reads linearly:

```zith
?*?(?i32 ! IoError)
```

Read left to right, outer to inner: an *optional* **pointer** to an *optional* **Result**, where the Result's success type is `?i32` and its error type is `IoError`.

### 8.2 `must` vs. `raw`

| | Debug mode | Release mode |
|---|---|---|
| `must` | Panics with file and line information. | The compiler guides you to remove it, turning it into an `if`/`else` with an early return and a custom error code. |
| `raw` | Always unchecked. | Always unchecked. |

```zith
let cfg: ?Config = tryLoad();
let c1 = must cfg;   // panics in debug; compiler warns/guides in release
let c2 = raw cfg;    // always unchecked; compiler always warns
```

`must` also doubles as an assertion: `must(cond)` panics with file and line info if `cond` is false (debug only). In release, the compiler guides you to replace it with proper error handling.

### 8.3 Propagation & Fallback

```zith
fn readConfig(path: string): Config! {
    let file = File.open(path)!
    let data = file.read()!
    parse(data)!
}

let name = ?user.name or "guest";
let data = !primary() or backup() or default;

// Propagation inside a chain
readFile("data.bin") -> parse(..)! -> validate(..)? -> process(..)
```

Accessing a failable type's inner value requires one of four operators: `?`, `!`, `raw`, or `must`.

- **`?`** unwraps an Optional — returns `T` or `null`.
- **`!`** unwraps a Result — returns `T` or an error.

#### Prefix vs Postfix

`?` and `!` serve two distinct roles depending on position:

| Position | Role | Rule |
|---|---|---|
| **Prefix** (start of expression) | Fallback | Only **one** `?` or `!` per expression. Must be followed by `or` to provide the fallback value. |
| **Postfix** (end of a chain segment) | Propagation | **Multiple** allowed. Propagates the error/null out of the chain, skipping the remaining calls. |

```zith
// Prefix — one per expression, must use 'or' for fallback
let x = ?opt or default;           // valid
let x = !result or fallback;       // valid
let x = ?opt or default or backup; // valid — chain of fallbacks

// Postfix — multiple allowed in a chain
let x = ?y or default.data()?fn()?process()?
//       ^prefix                                        ^postfix (propagation)

// Invalid — two prefixes in one expression
let x = ?y.?data.?fn();   // invalid
let x = ?opt?;            // invalid — '?' cannot follow another '?'
```

You can chain with `or` until it finds an Integral result (short-circuit).

### 8.4 `with` / `catch`

| Form | Behavior |
|---|---|
| `with` | Short-circuit — the first failure jumps straight to `catch` |
| `eager with` | Eager — every expression is evaluated; `catch` runs if any failed |

```zith
// Short-circuit
with (connectDb(), user: getUser(id)) {
    process(user);
}
catch (err) { log(err); }   // any name works; 'err' is convention

// Eager — all expressions run, then catch if any failed
eager with (a: fetchA(), b: fetchB()) {
    use(b);
} catch { log(a, b); }
```

> In `eager with`, all expressions are evaluated before `catch`. The named bindings (`a`, `b`) remain in scope inside `catch` so you can inspect which ones failed. In short-circuit `with`, only the failing expression is known, so `catch` receives a single error parameter.

### 8.5 `fail` Blocks

A `fail` block runs when an error would otherwise escape its associated scope. It can follow a named block (external) or sit inside a block as a scope guard (nameless):

```zith
// External fail
loadConfigure {
    let raw = readFile("config.json")!
    parse(raw)!
} fail loadConfigure(err) {
    if (err is NotFound) { continue(default); }
    throw Error{ context: "load failed", cause: err };
}

// Nameless fail -- guards the current scope
{
    fail (err) { log("scope error:", err); }
    risky()!
    another()!
}
```

> **Name linking:** an external `fail` block's name must match the block it guards. When there is only one failable block in scope, the name can be omitted. A nameless `fail` guards the current scope directly — the compiler passes the error the same way.

Inside a `fail` block, the parameter receives the error directly. You have four options:

- `continue(value)` — resume after the block with a replacement value.
- `return value;` — exit the enclosing function.
- `throw value;` — propagate a new error (requires the `Error` capability).
- Fall through — the original error propagates unchanged.

Use `@ok` to extract the success type from the failure node — useful when `continue` needs to return a value of a different type than the error:

```zith
fail (err) {
    continue(@ok err);   // extract success value from the failure node
}
```

`@err` also exists for extracting the error type in other contexts.

### 8.6 `throw`

```zith
fn divide(a: i32, b: i32): i32! {
    if (b == 0) throw DivisionByZero;
    a / b
}
```

---

## 9. Control Flow

### 9.1 Syntax Rules

Parentheses `()` are mandatory on every control structure's condition except function calls. Logical operators use English keywords; bitwise operators use standard symbols followed by `.`:

```zith
if (x > 0 and y < 10) { ... }
if isTrue() and (x > 5) { ... }
let mask = a &. b |. c ^. d;
```

### 9.2 `for`

```zith
for { ... }                                     // infinite
for (i in 0..=9) { @println(i); }               // inclusive range
for (i in 0..9)  { @println(i); }               // exclusive range
for (i = 0), (i < 10), (i += 1) { ... }         // init / cond / step
for (v in range(0, 100)) { @println(v); }       // over a generator

// Destructured group with fallback
let r = for ([acc, i]: i32), (i in 0..n) { acc *= i + 1 } or 0;
```

> If the loop body may never run, its return value is deduced as optional — unless `or` collapses it to a non-optional value.

User iterators expose `next(self)`. The canonical protocol returns `?T`: `null` ends the loop
and `Some(element)` is bound to the loop variable. Iterators that yield optional elements return
`??T`, binding the loop variable as `?T`; only the outer `None` is the iteration end.

> The init/cond/step form accepts comma-separated, parenthesized expressions — `for (i = 0), (i < 10), (i += 1)` — or the flat alternative, `for (i = 0, i < 10, i += 1)`.

### 9.3 Chain Flow (`->`)

The `->` operator pipes output left to right. The previous value is available as `..`, and tags capture values for later use. `!` and `?` propagate out of the chain normally. Precedence is left-to-right and lower than function calls.

```zith
getData() -> process(..) -> save(..);

getData()
    -> raw:    parse(..)
    -> parsed: validate(..)!       // ! propagates out of the chain
    -> connectDb()
    -> save(parsed);

// Inline block
readFile("data.bin")
    -> { let h = parse_header(..); validate(h)! }
    -> process_body(..);

// Comma sub-chain -- f1 and f2 receive foo's value but do NOT advance the chain
foo(), f1(..), f2(..) -> f3(..);

// Parenthesized sub-chain -- this one does advance inside the sub-chain
// But don't affect the main chain
foo(), ( f1(..) -> f2() ) -> f3(..);
         ^                      ^
         |                      |
         foo                    foo
```

> Comma sub-chains are useful for side effects — logging, validation — without disrupting the main data flow.

### 9.4 `state` Functions & State Machines

A `state` function is a normal function that belongs to a state machine. `dock` starts a state
machine and returns its eventual `state` return value; `jump` is a terminating transfer to
another state in the same machine. The compiler emits LLVM `musttail tailcc` calls for direct
state-to-state transitions and rejects old `flow`/`marker` spellings.

- **`state`**: `state Name(params): ReturnType { ... }` declares one state in a machine.
  All states in the same machine share the same return type; their parameter lists may differ.
  LLVM `tailcc` lets a `jump` between different signatures transfer without growing the call
  stack or introducing a hidden context frame.
- **`dock`**: `dock Start(args)` is an expression that calls a state and evaluates to the
  value returned by the last state in the machine. It may be assigned to a binding and
  returned by an ordinary function.
- **`jump`**: `jump Next(args);` terminates the current state and tail-calls the named state
  from the same machine. It is only valid inside a `state` body, validates against the target
  state's own parameter list, and lowers to `musttail tailcc` followed by `ret`.
- **`return`**: `return value;` inside a state returns from the whole state machine and gives
  the `dock` call its result. State functions are ordinary LLVM functions, and each transition
  is a direct `musttail` call immediately followed by `ret`.
- **`state(params): ReturnType` value**: a real state declaration can be stored in a value
  with the matching `state(params): ReturnType` signature. `dock` accepts that value with the
  same argument and return-type checks as a direct state call, and the call is emitted as an
  indirect `tailcc` call.

```zith
state Count(n: i32): i32 {
    if (n == 0) {
        return n;
    }
    jump Count(n - 1);
}

fn main(): i32 {
    let result = dock Count(3);
    return result;
}
```

```zith
state Machine(n: i32): i32 {
    return n;
}

fn main(): i32 {
    let S: state(i32): i32 = Machine;
    return dock S(42);
}
```

States can read module/global state and declare ordinary local bindings. The restrictions are
that every state in a machine must share the machine return type, `jump` must be the last
statement of a state block because it is a terminating transition, and `jump` arguments must
match the target state's own arity and parameter types.

#### State Machine Rules

| Rule | Detail |
|---|---|
| Return type | **Required.** Every `state` in the same machine has the same return type. |
| Parameters | **Diverging allowed.** States may use different parameter lists; `jump` and `dock` validate against the specific target state. |
| Entry | **Working.** `dock State(args)` reuses normal call argument coercion and returns the machine return type. |
| Transition | **Working.** `jump Next(args)` resolves a direct state symbol and lowers to a `musttail` call followed by `ret`. |
| Stack use | **Working.** Declarations and calls use LLVM `tailcc`; transitions compile to `musttail tailcc` calls, so recursive state loops and diverging transitions do not grow the stack on supported targets. |
| Scope | **Working.** State bodies are ordinary function bodies; they can use module/global state and local bindings. |
| Legacy syntax | **Rejected.** `flow fn`, `marker`, `stackful marker`, and old `dock { ... }` blocks are removed. |

State machines use `musttail tailcc`, so support is limited to targets that preserve tail-call
guarantees in the backend. Diverging parameters do not create an implicit context, frame, or
runtime blob. The compiler emits an unsupported-target diagnostic instead of claiming
arbitrary-target support.

---

## 10. Concurrency & Runtime APIs

### 10.1 Core-Language Position

Zith's core language does not define concurrency-specific statements, operators, or function kinds.
There are no dedicated HIR nodes for tasks, threads, `await`, or coroutine suspension. The compiler
understands only:

- ordinary declarations and calls;
- library-defined handle, channel, task, or executor types;
- traits/capabilities used to describe what those types guarantee;
- NRA facts about sharing, lending, capture, escape, and ownership across those calls.

### 10.2 Runtime Surface

The standard library or an alternate runtime may expose APIs such as thread spawners, executors,
message queues, join handles, or resumable tasks. Those APIs are library surface, not syntax:

```zith
let handle = runtime.spawn(workerFn, sharedData);
runtime.join(handle);

let task: Task<Response!> = runtime.schedule(fetchRequest);
let response = runtime.blockOn(task);
```

API names above are illustrative. The compiler does not reserve them.

### 10.3 What the Compiler Proves

Concurrency-related safety is enforced through the same pre-HIR ownership proof used everywhere
else:

- whether a call duplicates a resource illegally;
- whether a borrowed or `belong` value escapes;
- whether narrowing facts or branch facts justify later lowering decisions;
- whether shared/runtime-managed resources are passed only through the capabilities and wrapper types
  that define the contract.

The compiler does not special-case threads or async control flow. If a runtime API needs stronger
guarantees, it must express them through normal signatures, types, and traits that NRA can reason
about before HIR is finalized.

---

## 11. Comptime

Comptime covers compile-time computation: reflection, type manipulation, and `const` blocks.

### 11.1 Comptime Bindings


```zith
const counter: mut = 0;
counter += 1;   // valid at compile time
// counter += input().nextI32();   // COMPILE ERROR: frozen at runtime
```

### 11.2 `const` Blocks

A `const { ... }` block executes its contents at compile time. Every value inside must be computable at compile time — if anything depends on runtime input, the compiler reports an error.

`const fn` declarations are the future syntax for functions that resolve at compile time. Once
evaluation lands, they would be required to be called only inside a `const` block or assigned to a
`const` binding. Today the compiler only parses the declaration; evaluation is not implemented.

```zith
const result {
    let x = 10;
    let y = 20;
    x + y   // evaluated at compile time
};

import assets/data.json as Data;
const fn processJson(data: []char): JsonValue { ... }
const parsed = processJson(Data);  // runs at compile time
```

> Some macros and functions are overloaded to run at compile time; a compile-time `throw` halts compilation and displays the error message — equivalent to `static_assert` in other languages.

### 11.3 Reflection

Use `@` intrinsics to inspect types at compile time:

```zith
// Iterate the fields of a struct
for ( field in @fields MyStruct ) {
    @println("{}: {}", field.name, field.type);
}

// Check a type's kind
let isPrim = (T is @primitive);    // bool, i32, f64, etc.
let isStr  = (T is @struct);       // struct
let isComp = (T is @component);    // component
let isUn   = (T is @union);        // union
let isEn   = (T is @enum);         // enum

// Inspect field visibility
for ( field in @fields MyStruct ) {
    @println("{}: {}", field.name, field.visibility);  // pub, mod, private
}

// Check nullability
let nullable = (T is @nullable);   // ?T
```

### 11.4 Type Manipulation

> *This section is relevant for tooling authors and compiler contributors.*

You can create a type and modify it before it is "finalized":

```zith
// Create a new type
type Custom = @struct;

// Add fields -- allowed while the type is not yet returned or instantiated
@appendField Custom, x: i32;
@appendField Custom, y: f32;

// Remove a field
@removeField Custom, x;

// Add methods
@appendMethod Custom, fn distance(self): f32 { sqrt(self.x*self.x + self.y*self.y) }

// The type is "done" once it's returned or instantiated
let p: Custom = Custom { x: 1, y: 2.0 };

// Primitive aliases are IMMUTABLE -- they have no fields to modify
type Celsius = i32;
@appendField Celsius, x: i32;  // COMPILE ERROR: type is 'done' (primitive)
```

> A type built via `@struct` is "done" the moment it is returned or instantiated. Until then, `@appendField`, `@removeField`, and `@appendMethod` are available. Passing the type to a generic function also counts as "done."
>
> A type created with `type` (e.g. `type Celsius = i32`) is a primitive alias — it has no fields to modify and is always immutable. You can still add methods via `implement`, but you cannot `@appendField` or `@removeField`.

---

## 12. Assets

Assets are external files — JSON, images, other data — that the compiler validates and makes available at compile time.

### 12.1 Configuration

Declare asset paths in `ZithProject.toml`:

```toml
[project]
name = "my_game"
version = "0.1.0"

[assets]
assets = ["assets/", "../someOtherFolder"]
```

The compiler verifies that every declared path exists and is readable.

### 12.2 Importing Assets

```zith
import assets/data.json as Data;
import assets/sprites/player.png as PlayerSprite;
```

> `as` is mandatory here, to avoid conflicts between files that share a name but differ in extension.

Imported assets are available as compile-time constants.

### 12.3 Processing Assets at Compile Time

Combine assets with `const fn` to process them before the program runs:

```zith
import assets/config.json as ConfigData;

const fn parseConfig(data: []char): Config {
    // parse JSON at compile time
    JSON.parse(data)!
}

const CONFIG = parseConfig(ConfigData);   // runs at compile time
```

### 12.4 Runtime Assets

Assets not declared in `[assets]` are loaded at runtime with standard file I/O:

```zith
let runtimeData = fs.read("runtime/save.json")!;
```

---

## 13. Raw & Unsafe

### 13.1 Safety Hierarchy

```
Safe code (default)
    └─ needs raw blocks to call raw fn
         └─ needs to be inside a raw block / fn to use unsafe block
```

| | `raw` | `unsafe` |
|---|---|---|
| **What it is** | A base-level bypass, always unchecked. | A stronger bypass, valid only inside `raw` contexts. |
| **Scope** | Any expression or statement. | Only inside a `raw fn` or `raw` block. |
| **Debug mode** | Unchecked. | Unchecked. |
| **Release mode** | Unchecked; compiler warns. | Unchecked; compiler warns. |
| **Use case** | C interop, performance-critical paths. | Operations undefined if misused — pointer arithmetic, inline assembly. |

### 13.2 `raw fn`

A `raw fn` bypasses safety checks in both debug and release. The compiler warns if `raw` could be removed in release builds.

```zith
raw fn c_compat(x: raw opaque): raw opaque {
    // basic validations still exist but allows:
    // mut *, use raw opaque / union & etc..
}
```

### 13.3 `unsafe`

```zith
raw fn dangerous(x: opaque) {
    unsafe {
        // extension of raw — completely disables compiler checks
        // allows: bypass mutability, bit_cast, arbitrary address assignment,
        // inline assembly, etc.
        asm {
            mov rax, x         // x is a Zith var — compiler maps it
            call someFn         // can call Zith fns from asm
        }
    }
}
```

### 13.4 `Trust` as a Bridge

`Trust` bridges safe code to raw and unsafe code. A trait extending `Trust` may contain `raw fn` methods that are callable from safe contexts:

```zith
trait Place extends Trust {
    raw fn sample(): i32 {}
}

fn safe_caller(a: impl Place) {
    let v = a.sample();   // allowed: Trust is in scope via Place
}
```

### 13.5 When to Use Each

| Situation | Use |
|---|---|
| C interop, headerless libraries | `raw fn` |
| Pointer arithmetic, inline assembly | `unsafe` inside `raw fn` |
| Exposing low-level operations to safe code | `Trust` capability |
| Normal application code | Neither — stay safe |

---

## 14. Runtime: Polymorphism & Dynamic Behaviour

### 14.1 Static vs Dynamic Dispatch

By default, Zith uses static dispatch — the compiler knows the exact implementation at compile time. Zero overhead.

Use `dyn` for dynamic dispatch. At the call site you get polymorphism; the compiler and LLVM can often optimize away the indirection, making it zero-cost in practice.

### 14.2 `dyn` as a Type Hint

Like `union` (see [§3.6](03-type-system.md#36-union)), `dyn` works as a **type hint**. When the compiler can't deduce the concrete type, you write `dyn` to tell it you want dynamic behavior:

```zith
let x: dyn = 5;
if (x is i32) x += 32;   // smart-cast inside the branch

// dyn []T — dynamic Trait slice
let items: dyn []Drawable = shapes;
// dyn slice
let dynList: dyn [];

// In many cases LLVM strips the dyn overhead entirely
// even if not, compare the type is a simple int comparison
// in terms of performance, is union + ptr indirection, still fast
```

Inside a smart-cast branch (`is`), the type narrows to the concrete type. Mutations inside the branch affect the inner value; outside, assigning to the variable changes what the `dyn` points to (if `var`).

### 14.3 `dyn` Traits

`dyn Trait` is a `view` by default — a read-only, non-owning reference with a vtable. That means `view dyn` is redundant.

All other memory modifiers work with `dyn`:

| Keyword | `dyn` behavior |
|---|---|
| `view dyn` | Redundant — `dyn` is already a view |
| `share dyn` | Multiple names, same dynamic value |
| `lend dyn` | Exclusive mutable borrow of a dynamic value |
| `unique dyn` | Single-owner dynamic value |

```zith
fn draw_all(items: dyn []Drawable) {
    for (item in items) { item.render(); }
}

//specific verbose, you could use an interface or alias to simplify
fn modify(shape: lend dyn Drawable) {
    shape.scale(2.0);
}
```

`dyn` does **not** work with `interface` (see [§4.3](04-traits-interfaces.md#43-capabilities)) — only traits. Interfaces are structural and don't carry a vtable.

When you write a type that could be `dyn` or `opaque`, prefer `dyn` — it's short and clearer. Reserve `opaque` for cases where you specifically need `raw opaque` (untagged `void*`, C interop).

### 14.4 `dyn` vs Generics

| | Generics | `dyn` |
|---|---|---|
| Dispatch | Static — one copy per type | Dynamic — single code path |
| Code size | Larger (N copies) | Smaller (one copy) |
| Performance | No indirection | Vtable indirection (often elided by LLVM) |
| When to use | Hot loops, known types at compile time | Heterogeneous collections, plugins |

```zith
// Generic — compiler monomorphizes
fn log<T: Printable>(val: T) { val.print(); }

// dyn — vtable dispatch, LLVM may inline away the overhead
fn logDyn(val: dyn Printable) { val.print(); }
```

### 14.5 Object Safety

A trait is object-safe if all its methods meet these rules:

- No `Self` in parameter or return types (except `self`, `other`)
- No generic type parameters on the method
- No `Self: Sized` requirements

If you try to use a non-object-safe trait with `dyn`, the compiler rejects it.

---

## 15. Macros

| Type | Description |
|---|---|
| Normal (scoped) | Hygienic for bindings introduced by the macro, but template names are resolved from the call-site scope, so globals and imports remain visible when not shadowed. Requires the `@` prefix at the call site. |
| Raw macro | Inserts code literally at the call site; not hygienic. Names resolve in the call-site scope first and fall back to globals/imports. Also requires the `@` prefix. |
| Tag macro | HTML-like syntax. Tag attributes (e.g. `id=5`) are available as `attributes` when the macro is the first argument; content between tags forms the remaining arguments. Uses `<>` syntax — no `@` prefix. |

> Best practice: define macros inside a `context` block ([§17](17-contexts.md)) rather than activating them globally.

- They all have special arguments that can manipulate the AST. Default and raw macros accept attributes via `[capture]` syntax; tag macros receive attributes as `attributes`.

```zith
macro log(msg: expr) { @println("[LOG] ", msg); }

raw macro swap(a: identifier, b: identifier) {
    let _tmp = a; a = b; b = _tmp;
}

// Default/raw macro with capture attribute
@closure[capture](){ ... }

// Tag macro — attributes come from the tag syntax
<Section title="Overview"> body </Section>
<cool id=5, name="name"> content </cool>

// Macro parameter meta-types: identifier, expr, condition, body
```

### Scope and Hygiene

Normal macros keep macro-local bindings hygienic: a `let`/`var` introduced by
the template does not leak into the call site, and a call-site local does not
accidentally capture a same-named macro-local. Names that are not macro-locals
(e.g. a function or global referenced by the template) still resolve through
the call-site scope chain and can reach globals and imports.

Raw macros are literal: their `let`/`var` bindings and name reads use the
call-site scope. A raw macro name first resolves against call-site locals, then
the enclosing scopes, then the module/global scope. Splice statements remain in
the call-site block and can see names declared before or after the call in that
block.

The `::` scope-resolution operator remains a separate roadmap item; this
chapter describes only the default and raw macro resolution behaviour.

### 15.1 The `@` Prefix Rule

The `@` prefix is what distinguishes a macro call from an ordinary function call:

```zith
// Macro call -- @ prefix
@println("hello");
@log("debug message");
@serialize(obj);

// Function call -- bare name
console.write("hello");
process(data);
save(file);
```

Tag macros are the one exception — they use `<>` syntax and never take the `@` prefix:

```zith
<div class="container"> content </div>
<Section title="Overview"> body </Section>
```

---

## 16. Words (Custom Operators)

Words let you define custom operators from identifiers. Each word has a fixed position — **prefix**, **infix**, or **suffix** — with language-defined precedence.

- You must activate a word with `use`, even if you already imported its module.
- Two words with the same name in the same scope: compile error.
- If the compiler sees any ambiguity (even potential), it errors out.
- Best practice: define words inside a `context` ([§17](17-contexts.md)).

### 16.1 Word Types

| Type | Description | Example |
|---|---|---|
| `operator` | Overload a specific operator (`+`, `-`, `*`, `()`, etc.) | `implement Vec3 as Arithmetic { fn +(self, other: Self): Self { ... } }` |
| `token` | A word with low precedence that does nothing alone. Serves as a named argument for macros and other words. | `token SELECT;` |

#### Operator Words

Operator words overload built-in operators. Use `implement` with a capability to define the behavior:

```zith
implement Vec3 as Arithmetic {
    fn +(self, other: Self): Self { ... }
    fn -(self, other: Self): Self { ... }
    fn *(self, scalar: f32): Self { ... }
}

// Custom word — not overloading a built-in operator
use math.vec.dot as DOT;
use math.vec.cross as CROSS;

// Infix — reads as dot(vec1, vec2)
let d = vec1 DOT vec2;

// Prefix — reads as VALIDATE input
let result = VALIDATE data;

// Suffix — reads as input CHECK
let value = input CHECK;
```

| Position | Reads as |
|---|---|
| Infix | `a DOT b` → `dot(a, b)` |
| Prefix | `not x` → `not(x)` |
| Suffix | `x!` → `assert(x)` |

#### Token Words

Token words have low precedence and do nothing alone. They serve as named arguments for macros and other words — e.g., SQL keywords:

```zith
token SELECT;
token FROM;
token WHERE;

// Operator* defines behavior for a token
operator* (SELECT, list) { ... }
```

> Tokens are useful for DSLs where keywords need to be passed as arguments without function call syntax.

### 16.2 Words vs. Macros

- **Macros:** Better for heavy logic, still require `()` syntax, can't return values.
- **Words:** Work as keywords, let you return values, and can delegate to macros. Better for lightweight tasks.

> Words let you pass keywords, words, and macros as arguments.

---

## 17. Contexts

A context bundles macros, constants, words, and other declarations into a reusable package. You can apply it to a single block or activate it globally — only one context may be active at a time in a given scope.

```zith
// Scoped: macros and words active only inside this block
use SQL {
    SELECT * FROM users WHERE id = :id
}

// Global: replaces any previous active context
use SQL;
```

You can attach a context to any named block in Zith.

### Best Practice

Define your macros and words inside context blocks rather than leaving them globally active. Think of it as a lightweight namespace for DSLs — keeps the rest of your code clean.

---

## 18. C Interop

Zith offers three modes of C interop, designed to make using existing C libraries as frictionless as possible.

### 18.1 Automatic Binding via `.h`

Including a C header automatically generates bindings. Every function becomes a `raw fn` by default, and pointer types are inferred:

```zith
import "openssl/ssl.h";

// All C functions are now available as raw fn
SSL_CTX_new(method);
```

| C type | Inferred Zith type |
|---|---|
| `T*` | `mut *T` (mutable pointer) |
| `const T*` | `*T` (read-only pointer) |
| `T**` | `mut *mut *T` |
| `void*` | `raw opaque` |
| `int`, `float`, etc. | direct primitive equivalents |

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

## 19. Project Configuration

### 19.1 `ZithProject.toml` (per-project)

```toml
[project]
name    = "my_app"
version = "0.1.0"

[build]
runtime = true            # default: true; set false for OS/embedded targets
asm     = "x86_64_intel"  # required if using inline assembly
                          # errors if it diverges from the host machine or other project files

[assets]
paths = ["assets/"]       # compile-time-validated asset paths

[dependencies]
std = "bundled"
```

### 19.2 `ZithFlags` (compiler / global)

```
--runtime=false     # disable the runtime globally: removes most allocators,
                    # disables `must`, disables dynamically linked libraries and
                    # anything that depends on them, and forces all available
                    # std to be statically linked
--asm=arm64         # set the assembly dialect globally
--release           # release mode 
--debug             # debug mode (default)
```

> `runtime = false` disables the heap, standard stack assumptions, and signal handlers. Any standard library feature that requires a runtime becomes unavailable at compile time.

---

## 20. Standard Library

### 20.1 Three-Part Structure

| Namespace | Stability | Use when |
|---|---|---|
| `std` | Stable, backward-compatible | You need a guaranteed API |
| `soon` | Experimental, may change | You're prototyping and don't mind breakage |
| `c` | Direct C FFI bindings | You need to call C APIs |

```zith
import std;
import soon;   // use with caution — API may shift
import c;       // raw C bindings
```

### 20.2 Core Modules

#### `std/io/console`
```zith
fn println(msg: []char): void;
fn print(msg: []char): void;
fn eprint(msg: []char): void;
```

```zith
@println("hello");
```

#### `std/collections/DynArray`

```zith
struct DynArray<T> {
    fn push(self: lend, val: T);
    fn pop(self): ?T;
    fn len(self): u64;
    fn get(self, index: u64): ?T;
}
```

#### `std/fs`
```zith
struct File { ... }

fn open(path: string): File!;
fn read(self: view File): []u8!;
fn write(self: lend File, data: []u8): void!;
```

### 20.3 Common Traits

| Trait | What it enables |
|---|---|
| `Copy` | Bitwise copy — primitives and components get this by default |
| `Clone` | `fn clone(self): Self!` |
| `Lent` | Can appear as a `lend` parameter |
| `Share` | Safe to share across threads |

---

## 21. Best Practices & Patterns

### 21.1 Ownership Patterns

- **Resources shall be `unique`:** `let resource: unique = Resource.new();`
- **Use `share` for intentional multiple owners:** implement `Share` and `Clone` explicitly.
- **Use `view` for reading:** `fn process(config: view Config)`
- **Use `lend` for temporary mutation:** `fn update(state: lend GameState)`
- **Use `belong` for part-of relationships,** such as back-pointers.

### 21.2 Optional & Failable Patterns

- **Prefer `?...or` for optionals:** `let name = ?user.name or "guest";`
- **Prefer `!...or` for failables:** `let config = !loadPrimary() or loadBackup() or defaultConfig();`
- **Reserve `must` for initialization:** `const API_KEY = must env("API_KEY");`

### 21.3 Context Patterns

- Group related DSL features by defining macros and words inside a single `context` block.
- Activate at most one global context per domain to avoid pollution.

### 21.4 Error Handling Patterns

- Add context to errors using `fail` blocks and custom `throw` statements.
- Chain fallbacks explicitly: `let data = !step1() or step2() or step3() or AllFailed;`

### 21.5 Macro Patterns

- Prefer macros scoped inside contexts over global activation.
- Prefer to apply context per block for the same reason.

### 21.6 Rule of Three

If a function needs more than three specialized tools (state machines, words, contexts, macros, comptime, inline error handling), something went wrong. Split the function or reconsider your abstraction.

```
// Good — two tools: state machine + word
state Init() {
    jump Ready();
}
fn process() {
    dock Init();
    step1 -> step2
}

// Warning sign — four tools in one function
fn process() {
    dock spinning();           // state machine
    use Math;                  // context
    use assert AS CHECK;       // word
    risky()!                   // inline error handling
    // Prefer: move the context/word usage to a wrapper function
}
```

The Rule of Three keeps code readable. Zith gives you many tools — you don't have to use them all at once.

### 21.7 Naming Conventions

| Construct | Convention | Examples |
|---|---|---|
| Variables & functions | camelCase | `playerHealth`, `getDamage`, `loadConfig` |
| Components | single word, lowercase | `rgb`, `color`, `file`, `vertex`, `health` |
| Structs | PascalCase | `Point`, `Container`, `DynArray`, `GameConfig` |
| Traits & interfaces | PascalCase | `Printable`, `iPositioned` (interfaces use lowercase `i` prefix) |
| Files | kebab-case | `game-loop.zith`, `asset-manager.zith` |
| Constants & comptime | UPPER_SNAKE_CASE | `MAX_SIZE`, `PI`, `DEFAULT_TIMEOUT` |
| Enums | PascalCase for the type; PascalCase for variants | `enum Direction { North, South }` |

---

## 22. Appendix — Keyword & Operator Reference

### 22.1 Keywords & Operators

| Keyword | Category | Summary |
|---|---|---|
| `import` / `from` / `export` | Module | Import / inject into scope / re-export. |
| `alias` | Module | Name alias for a type, namespace, or symbol. |
| `use` | Module | Bring a word, context, or operator into scope. |
| `type` | Types | Distinct type copy, or a compile-time constraint (with `or`). |
| `as` | Types | Cast / coercion. Also used in `implement T as Trait`. |
| `is` | Types | Type check / narrowing. Boolean. Supports `@struct`, `@nullable`, etc. |
| `enum` | Types | Closed compile-time constants — C-style, struct-backed, or ADT-style. |
| `union` | Types | Runtime tagged union; variants separated by commas. |
| `struct` | Types | Record type. Fields may be grouped with `[]`. |
| `component` | Types | POD / copy-by-default struct. No traits. C-compatible. |
| `implement` | Types | `implement T {}` or `implement T as Trait {}`. |
| `when` | Types | Pattern matching — ranges, type dispatch, branch tags, `..` to ignore fields. |
| `[]T` / `[N]T` / `[_]T` | Types | Slice / fixed array / deduced-size array. |
| `\| \|` | Types | Pack — named tuple / variadic / closure capture group. |
| `pub` / `mod` / `mod(..)` / `mod(N)` | Visibility | Public / module-local, with optional depth. |
| `let` / `var` / `global` / `const` | Bindings | Immutable / mutable / static storage / compile-time constant. |
| `default` / `lend` / `view` / `unique` / `share` / `belong` | Memory | NRA memory modifiers — `default` is implicit when no keyword is written ([§7](07-memory-model.md)). |
| `fn` / `const fn` / `state` / `raw fn` / `extern fn` | Functions | Five exclusive function kinds; cannot be combined. |
| `trait` / `interface` / `extends` / `requires` / `dyn` | OOP | Nominal traits, structural interfaces, extension, constraints, dynamic dispatch. |
| `Copy` / `Functor` / `Arithmetic` / `Error` | Capabilities | Operator and behavior capabilities. |
| `Null` / `Fail` | Capabilities | Negative — activate only in proven-invalid states. |
| `Allocator` / `Generator` / `Share` / `Lent` / `Trust` / `Unique` | Capabilities | Memory, runtime protocol, and safety capabilities. |
| `state` / `dock` / `jump` | State machines | `state` declarations, a state entry call, and terminating transitions. |
| `->` / `..` | Chain | Chain flow / placeholder for the previous value. Left-to-right. |
| `,` (in a chain) | Chain | Sub-chain — applies but does not advance the main chain value. |
| `operator` / `token` | Words | Custom operator definition / token word definition ([§16](16-words.md)). Must be defined inside a `context` — global operator overloading is prohibited. |
| `?T` / `T!` | Errors | Optional / Result types. May be stacked. |
| `?` / `!` (postfix) | Errors | Propagate Option / Result. No semicolon. Propagate out of chains. |
| `or` | Errors / Loops / Types | Fallback / collapse an optional loop return / type constraint separator. |
| `must` | Errors | Panic in debug; guided removal in release. |
| `raw` | Errors / Raw | Always unchecked, in both debug and release. Compiler warns in release. |
| `unsafe` | Raw | Stronger than `raw`; valid only inside raw contexts. |
| `throw` / `fail` / `continue(v)` / `with` / `eager with` | Errors | Explicit throw, scoped recovery, resume, bundled fallible operations. |
| `::` | Operators | Scope resolution — access a shadowed outer name. |
| `and` / `or` / `not` / `xor` | Operators | Logical (English keywords). |
| `&.` / `\|.` / `^.` / `~` / `<<` / `>>` | Operators | Bitwise. |
| `@` / `#` | Annotations | `@` for intrinsics, reflection, and the macro prefix. `#` for variable/field attributes. |
| `extern 'C'` | Interop | C binding — automatic via `.h`, manual, or external. |
| `runtime` / `asm` | Config | `ZithProject` / `ZithFlags` build settings. |
| `assets` | Config | `ZithProject.toml` asset path declarations. |

### 22.2 Compiler Intrinsics

| Intrinsic | Summary |
|---|---|
| `@fields T` | Iterate the fields of a type. |
| `@sizeOf T` | Size of a type, in bytes. |
| `@canonicalType T` | Stable canonical type identity, returned as `u128`. |
| `@hasTrait T, Trait` | Check whether a type implements a trait. |
| `@struct` / `@component` / `@union` / `@enum` | Type-kind checks, used with `is`. |
| `@nullable` | Check whether a type is nullable (`?T`). |
| `@primitive` | Check whether a type is a primitive. |
| `@allocate T, data` | Allocate within the current memory region. |
| `@pack` | Extract or inject a closure's capture pack. |
| `@toStruct pack` | Convert a pack to a plain struct. |
| `@toPack struct, region` | Convert a struct back to a pack. |
| `@appendField Type, name: T` | Add a field to a type being constructed. |
| `@removeField Type, name` | Remove a field from a type being constructed. |
| `@appendMethod Type, fn ...` | Add a method to a type being constructed. |
| `@file` / `@line` / `@fnName` | Location information. |
| `@location` | Rich panic message source. |
| `@ok` / `@err` | Retrieve the T or E from `catch` & `fail`. |

### 22.3 Attributes

| Attribute | Summary |
|---|---|
| `#volatile` | The variable is volatile; the compiler must not optimize it away. |
| `#thread_local` | The variable uses thread-local storage. |

---

*Zith Language Specification — Draft v0.9 — Subject to change*
