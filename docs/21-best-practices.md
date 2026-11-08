## 21. Best Practices & Patterns

> **Scope:** this chapter describes Zith. Zith and `Zith--` are sibling
> languages with separate contracts. The patterns below use `%T`, `&T`,
> `&mut T`, `^T`, `try ... or`, `fail`, `catch`, `resume`, `use`, `risky()!`,
> and `assert AS CHECK`. These are Zith design examples, not `Zith--`
> requirements. Read [`Zith--.md`](Zith--.md) for that language's contract and
> [impl-status.md](impl-status.md) for the compiler's current support.

### 21.1 Ownership Patterns

- **Use `%T` for owned resources:** `let resource: %Resource = Resource.new();`
- **Use `&T` for read access:** `fn process(config: &Config) { ... }`
- **Use `&mut T` for writable access:** `fn update(state: &mut GameState) { ... }`
- **Use `^T` for a stored non-owning lifetime dependency:** keep its target
  alive at a stable address. Prefer `&T`/`&mut T` for parameters; a `^`
  parameter can retain its argument, so reserve it for a domain reason.
- **Use the `Share` capability for values that may cross thread boundaries.**

### 21.2 Invalid-State Patterns

- **Prefer `try ... or` for any invalid state:** `let config = try loadPrimary() or loadBackup() or defaultConfig();`
- **Keep a `try` result when you need to inspect it later:** bind it to a local and test it with `is @ok`.
- **Keep absence distinct from errors:** `Nil` is invalid, but does not implement `Error`.
- **Use `fail` to inspect an error value:** `fail (err) { log(err); }`
- **Reserve `must` for cases where invalidity is fatal:** `const API_KEY = must env("API_KEY");`

### 21.3 Context Patterns

- Reserve contexts for APIs that deliberately integrate with domain-specific syntax, such as Math, SQL, or HTML.
- Do not use contexts as generic namespaces or containers for ordinary public APIs.

### 21.4 Error Handling Patterns

- Use `or` for fallbacks across any invalid state. It evaluates a fallback only after invalidity
  and retains the last invalid result if every alternative is invalid.
- Bind a `try` result when later code needs to test its state with `is @ok`.
- Use `catch` only for invalid values produced during `with` initialization.
- Use `fail` only for invalid values whose types implement `Error`, and use `resume value;`
  to continue with a replacement result.

### 21.5 Context and Tag Patterns

- Reserve contexts for APIs that deliberately integrate with domain-specific syntax.
- Declare a tag's body kind to match the syntax the domain API needs to consume.

### 21.6 Rule of Three

If a function needs more than three specialized tools (state machines, words, contexts, tags, comptime, inline error handling), something went wrong. Split the function or reconsider your abstraction.

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

The Rule of Three keeps code readable. Zith gives you many tools. You don't have to use them all at once.

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

### 21.8 Universal Public API Style

The universal API style is the shared design convention for public Zith APIs.
It is separate from a project's feature policy. Multiple implementation styles
can expose the same API, as in the three `classify(score: i32): i32` examples
in the README.

Prefer tuples over output parameters for multiple return values. Do not return
compile-time `type` values, raw function pointers, or `dyn fn` directly from
ordinary APIs unless the domain requires them. Use generic trait and interface
bounds rather than `dyn` dispatch in the universal style. Keep
`@ensure`, `maybe`, and `assume` internal unless callers need them to
understand a specific API contract. Use `camelCase` for method names.

Prefer `&T` and `&mut T` for reference parameters, and reserve `^T`/`^mut T`
for stored dependencies and return values. At a call site every argument whose
mode is not a plain value move is marked: an immutable mode with its sigil
(`&x`, `^x`, `%x`) and a mutable mode with its keyword (`lend x`, `bind x`,
`own x`); see
[ADR 0038](adr/0038-call-site-reference-annotations.md).

These are conventions rather than compiler restrictions. A public API may
depart from them when its purpose justifies the added specialization. See
[ADR 0032](adr/0032-universal-api-project-identity-and-contexts.md).

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
