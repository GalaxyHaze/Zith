## 21. Best Practices & Patterns

> **Note:** Best practices in this chapter describe the intended programming model. Many underlying
> features (NRA, `dyn`, `comptime`, `when`) are **spec-only**. Verify feature availability in
> [impl-status.md](impl-status.md) before applying a pattern.

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

If a function needs more than three specialized tools (markers, words, contexts, macros, comptime, inline error handling), something went wrong. Split the function or reconsider your abstraction.

```
// Good — two tools: marker + word
flow fn process() {
    dock init { ... }
    step1 -> step2
}

// Warning sign — four tools in one function
flow fn process() {
    dock init { ... }          // marker
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

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
