## 15. Macros

> **Implementation status:** `@macro` calls are a **parse error** — `@` in expression position is
> not handled by the parser. Tag macros (`<Tag>`) are **spec-only**.
> See [impl-status.md](impl-status.md).

| Type | Description |
|---|---|
| Normal (scoped) | Hygienic — symbols do not leak into the call site's scope. Requires the `@` prefix at the call site. |
| Raw macro | Inserts code literally at the call site; not hygienic. Also requires the `@` prefix. |
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

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
