## 3. Type System

> **Implementation status:** Primitive types, structs, enums, unions, `?T`, `T!`, `[N]T`, `[]T`,
> pointers, type aliases, `implement`, and generics (`<T>`) are all **working**. `as` casting is
> **working** for numeric conversions only, and is required: there are no implicit conversions
> between numeric types (a numeric *literal* still adapts to its annotated type). Pointers are
> non-nullable — `null` requires `?*T` — and `*void` is rejected in favour of `raw opaque`.
> `is` is limited to `(val is null)` on an optional; union narrowing, `when` pattern matching,
> `dyn Trait`, and NRA ownership modifiers (`lend`, `view`, etc.) are **spec-only**.
> See [impl-status.md](impl-status.md).

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

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
