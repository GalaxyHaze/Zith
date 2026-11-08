## 11. Comptime

> **Status:** see [impl-status.md](impl-status.md) for the current status of comptime features.
> `const` bindings are immutable at runtime but are not evaluated at compile time. `comptime`
> blocks, reflection intrinsics, and `const fn` evaluation are not implemented.


Comptime covers compile-time computation: reflection, type manipulation, and `const` blocks.

### 11.1 Comptime Bindings

A `const` value is bare: it carries no access or lifetime qualifier and no
`mut`, because the intent is that it can be copied directly. A `const` binding
that needs to be modified during evaluation uses a `var` inside a
compile-time context instead:

```zith
const total {
    var counter = 0;
    counter += 1;   // valid at compile time
    // counter += input().nextI32();   // COMPILE ERROR: frozen at runtime
    counter
}
```

`const X: view i32 = 5` and `const X: mut = 0` are both errors. The value
stored as a `const` has to be bare. See
[ADR-0057](adr/0057-bare-union-members-and-const-values.md).

### 11.2 `const` Blocks

A `const { ... }` block executes its contents at compile time. Every value inside must be computable at compile time. If anything depends on runtime input, the compiler reports an error.

`const fn` declarations are the future syntax for functions that resolve at compile time. Once
evaluation lands, they would be required to be called only inside a `const` block or assigned to a
`const` binding. Today the compiler only parses the declaration. Evaluation is not implemented.

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

> Some macros and functions are overloaded to run at compile time. A compile-time `throw` halts compilation and displays the error message, which is equivalent to `static_assert` in other languages.

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
> A type created with `type` (e.g. `type Celsius = i32`) is a primitive alias. It has no fields to modify and is always immutable. You can still add methods via `implement`, but you cannot `@appendField` or `@removeField`.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
