## 16. Words (Custom Operators)

> **Implementation status:** `prefix`, `suffix`, `infix`, and `nop` declarations are **parse
> skipped** — the declaration is accepted but the body is dropped via `skipDelimited`. Word call
> expressions are a **parse error**. No semantic or HIR support.
> See [impl-status.md](impl-status.md).

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

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
