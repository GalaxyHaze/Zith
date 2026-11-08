## 3. Type System

> **Status:** see [impl-status.md](impl-status.md) for the verified status of each type form.
> `as` casting is required for numeric and nominal-wrapper conversions: there are no implicit
> conversions between numeric types, though a numeric *literal* still adapts to its annotated
> type. Pointers are non-nullable, so `null` requires `?*T`, and `*void` is rejected in favour of
> `raw opaque`. Full NRA ownership modifiers (`own`, `share`, `belong`) remain full-Zith work.
>
> This chapter preserves the current Zith-- spellings in its implementation examples. Full-Zith
> uses `pod` for the POD declaration and `extend T with Trait` for nominal trait conformance,
> as recorded in [ADR-0040](adr/0040-full-zith-surface-names-and-keyword-limit.md).

### 3.1 Primitive Types

| Category | Types |
|---|---|
| Unsigned integers | `u8`, `u16`, `u32`, `u64`, `u128` |
| Signed integers | `i8`, `i16`, `i32`, `i64`, `i128` |
| Floats | `f32`, `f64` |
| Other primitives | `bool`, `char`, `void` |
| Compiler-internal | `unknown` — a valid but unresolved type, not user-instantiable. `invalid` — a dead or uninitialized state (a moved variable, a proven-null variable). Neither can be named or stored by user code. |
| Special | `never`, `null` |
| Opaque | `opaque` — an erased reference type, equivalent to a tagged `void*`. It has no bare form: a bare `opaque` is an error and the reference is written, `&opaque` or `lend opaque`. `raw opaque` is an untagged `void*` and is a plain value, used for C interop. See [ADR-0056](adr/0056-nra-erased-forms-and-spelling.md). |

### 3.2 Slice & Array Types

| Syntax | Meaning |
|---|---|
| `[]T` | Slice — a fat pointer (pointer + length). String literals are `*char` and adapt to `[]char`. |
| `[N]T` | Fixed-size array, stack-allocated. |

#### Strings

`char` is a UTF-8 code unit. A string literal has type `*char` and adapts
implicitly to `[]char` where a slice is expected. There is no built-in `string`
type in Zith--; use `[]char` or `*char`. The origin-tracking rules in
[§7.1](07-memory-model.md#71-what-nra-tracks) describe the full-Zith design.

```zith
let text: []char = "hello";
```

String literals concatenate with `+`, which produces an allocator-origin value.

### 3.3 Enum

An enum is a closed set of named compile-time constants. A concrete declared
value type is shared by all constants. Use `enum: union` when named constants
need heterogeneous union members.

#### C-style
```zith
enum Direction { North, South, East, West }
enum Status: i32 { Ok = 0, Err = 1, Pending = 2 }
```

#### Struct-backed
```zith
enum Colors: RGBA {
    red   = { 255, 100, 0, 255 },
    blue  = { 60, 80, 240, 255 },
    green = { 80, 255, 80, 255 },
}
```

### 3.4 Struct

#### Field Declaration & Grouping

Use individual fields for unrelated fields, and `[]` groups for semantically related fields that share a type:

```zith
struct Sample { name: *char, age: i32 }

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
    next: ?own Self,   // owns next; null at tail
    prev: ?belong Self,   // back-ref; lifetime tied to parent; null at head
}
```

#### Implementation Blocks

Structs, enums, and unions can declare methods without bodies in the type definition. The implementation goes in an `implement` block:

```zith
// Struct — declares methods, no body
struct Node<T> {
    data: T,
    next: ?own Self,
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

Primitives, `?T`, and `[]T` can also be the owner of an `implement` block. Methods on these owners use the same implicit `self` receiver and can be called with the normal `.method()` syntax:

```zith
trait Numeric {
    fn double(self): i32;
}

implement i32 as Numeric {
    fn double(self): i32 { return self * 2; }
}

trait Present {
    fn present(self): bool { return self is null; }
}

implement ?char as Present {}

trait ByteLen {
    fn len(self): i32;
}

implement []u8 as ByteLen {
    fn len(self): i32 { return 4; }
}
```

`implement` owners are limited to primitives, `?T`, `[]T`, and named types in this iteration. Pointer and fixed-array owners (`*T`, `[N]T`) are rejected.

Components define method bodies inline, so they cannot use `implement`:

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
    fn lengthSquared(self): f32 { self.x*self.x + self.y*self.y }
    fn dot(self, other: Self): f32 { 
        self.x*other.x + self.y*other.y 
    }
}
```

A component must satisfy all of the following constraints:

- Every field is a primitive, another component, or an array/slice of either. There are no structs, unions, or non-integer enums.
- No trait declarations (`component Name: Traits` is not allowed).
- Inline functions are permitted but restricted to pure transformations:
  - No side effects (I/O, allocation, global mutation).
  - Bodies may use literals, parameters, arithmetic, comparisons, and field access. Calls are
    rejected because Zith-- does not prove that a callee has no side effects.
  - Must return a value, so `void` is not allowed.
- Copying is always bitwise (memcpy-safe).
- Layout is C-compatible, so there are no vtables or fat pointers.
- No self-referential fields (`?own Self`, `?belong Self`).

### 3.6 Union

Use a `union` for a runtime-tagged value that holds one of several member types.
Its members can be heterogeneous:

```zith
union Numbers {
    i32, f32, u64
}
```

A `union` member is bare: it carries no access or lifetime qualifier. The
qualifier belongs to the union value, not to an alternative inside it. `own`
and `&` on a member are errors; the lifetime qualifier `bind` (`^mut`) and the
read bind `^` are allowed, because a stored back-reference is intrinsic to the
member. See [ADR-0057](adr/0057-bare-union-members-and-const-values.md).

```zith
union Incorrect { own Point, &i32, bind Node }   // error
union Correct   { Point, i32, bind Node }

let x: mut Correct;    // the qualifier lives on the value
let y: &Correct;
```

Use a type hint when an expression needs to produce a union:

```zith
enum Flag { A, B, C }
let flag = Flag.A;

let value: union = when (flag) {
    A = 42,
    B = 3.14,
    C = true,
};
```

> Without an explicit `union` type hint, the compiler cannot deduce a union. It must be stated explicitly.
>
> `dyn` works the same way as a type hint. See [§14.2](14-polymorphism.md#142-dyn-as-a-type-hint).

`raw union` is an untagged C-style union, valid only inside `raw` contexts. Accessing the wrong variant is undefined behavior.

Use `enum: union` for a closed set of named compile-time constants whose
values can have different types:

```zith
enum ADT: union {
    One = Point{5, 5, 0},
    Str = "lol",
    F32 = 0.5,
}
```

#### Named Union Variants

A tagged union can name each alternative and associate it with a payload shape:

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

> Both `union` & `enum` can have methods, but neither can implement traits.
>
> Pack literals in `enum:union` variants are treated as anonymous structs with a concrete layout.

### 3.7 Union Narrowing (`is`) & Flow Typing

The `is` operator narrows a union within a branch. Branches are isolated. Moves inside one branch don't affect others. After the block completes, the type **widens back** to the full union. The underlying tag does not revert. The value stays `i32` internally, but the type system treats it as the full union again.

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
fn serialize<T: Serializable + Printable, U: Clone>(val: T, ctx: U): *char { ... }

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

### 3.10.1 Nominal Types (`type Name = T`)

`type Name = T` declares a nominal one-field wrapper. In Zith-- the wrapper is
not interchangeable with `T`; construction and extraction are explicit `as`
casts:

```zith
type UserId = i32
alias Number = i32

fn makeId(n: i32): UserId { return n as UserId; }
fn readId(id: UserId): i32 { return id as i32; }

fn main(): i32 {
    let id: UserId = makeId(7);
    return readId(id) + (id as i32);
}
```

Contract:

- `T as Name` is explicit nominal construction.
- `Name as T` is explicit extraction of the underlying value.
- A value of `Name` does not unify with its underlying `T` and vice versa; use
  the corresponding `as` cast when the boundary is intentional.
- `alias Name = T` remains transparent, so `Name` and `T` unify without casts.
- There is no nominal field-access syntax in Zith--; a nominal value is a
  distinct type, not a struct literal.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
