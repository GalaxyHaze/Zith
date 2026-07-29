## 17. Contexts

> **Implementation status:** `context` declarations and `use` activation are **parse skipped** —
> declarations parse but bodies are dropped. No semantic support. See [impl-status.md](impl-status.md).

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

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
