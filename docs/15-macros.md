## 15. Macros

> **Implementation status:** `@macro` and `raw macro` calls are implemented and expand through
> sema/HIR. Tag macros (`<Tag>`) are implemented for statement-position expansion with named
> attributes; they do not produce a value.
> See [impl-status.md](impl-status.md).

| Type | Description |
|---|---|
| Normal (scoped) | Hygienic for bindings introduced by the macro, but template names are resolved from the call-site scope, so globals and imports remain visible when not shadowed. Requires the `@` prefix at the call site. |
| Raw macro | Inserts code literally at the call site; not hygienic. Names resolve in the call-site scope first and fall back to globals/imports. Also requires the `@` prefix. |
| Tag macro | HTML-like syntax. Tag attributes must be `name: value` pairs and are available as `attributes.name` in the body. Content between tags is parsed as statements and passed as one `body` argument. Uses `<>` syntax — no `@` prefix. |

> Best practice: define macros inside a `context` block ([§17](17-contexts.md)) rather than activating them globally.

- Macros accept a special first parameter named `attributes` (with no meta-type) to receive
  call-site attributes. `@closure|k: 1|(...)` and `<Box k: 1> ... </Box>` both expose the values
  as `attributes.k` inside the body. A macro without that parameter rejects attribute syntax.

```zith
macro log(msg: expr) { @println("[LOG] ", msg); }

raw macro swap(a: identifier, b: identifier) {
    let _tmp = a; a = b; b = _tmp;
}

// Default/raw macro with attribute parameter
macro closure(attributes, body1: block) { body1; }
@closure|k: 1|({ ... })

// Tag macro — attributes come from the tag syntax
<Section title: "Overview"> body </Section>

// Macro parameter meta-types: identifier, expr, condition, block, body
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
<Section title: "Overview"> content </Section>
```

### 15.2 Tag Macro Declarations

`tag macro` is a macro declaration that is invoked with HTML-like syntax. The
macro body is a template; only the tokens cloned at a call site are compiled:

```zith
tag macro Section(attributes, content: body) {
    let title = attributes.title;
    content
}

<Section title: "Overview">
    section body
</Section>
```

Attributes are optional and must be named `attributes` as the first parameter.
Every attribute is `name: value` (no `=` form) and is substituted wherever the
body uses `attributes.name`. Tag macros cannot be used as expressions.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
