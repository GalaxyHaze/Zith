## 2. Module System

> **Implementation status:** `import`, `from`, `export`, `alias`, `type`, and `pub`/`mod` visibility
> are **working**. `use` (word/context activation) is **parse skipped** — the body is accepted but
> has no semantics. `::` scope resolution is not implemented in the modern pipeline; qualified
> namespace access via `.` is working for imported modules, types and members. See
> [impl-status.md](impl-status.md).

### 2.1 Import Keywords

| Keyword | Behavior |
|---|---|
| `import path` / `import path as name` | Imports a module under its first path segment as a namespace, or under an explicit alias. Members are accessed as the full dotted path (`std.io.console.println`) or `alias.symbol` |
| `from path` | Injects all visible symbols directly into scope, sugar for simple cases |
| `export path` | Re-exports a dependency; consumers of this module receive it as well |

```zith
import std/io/console;
std.io.console.println("full path");

import std/io/console as out;
out.println("alias");

from std/io/console;
println("direct");

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

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
