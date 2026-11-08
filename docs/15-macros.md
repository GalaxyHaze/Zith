## 15. Macros

> **Status:** see [impl-status.md](impl-status.md) for the current status of macros. Zith--
> implements `macro` and `raw macro`, and rejects full-Zith `tag` declarations with `E2010`.

| Type | Description |
|---|---|
| Normal (scoped) | Hygienic for bindings introduced by the macro, but template names are resolved from the call-site scope, so globals and imports remain visible when not shadowed. Requires the `@` prefix at the call site. |
| Raw macro | Inserts code literally at the call site; not hygienic. Names resolve in the call-site scope first and fall back to globals/imports. Also requires the `@` prefix. |

> **Full-Zith distinction:** tags are not macros. `@<` is a dedicated tag-opening delimiter, not
> an intrinsic call or an `@` prefix applied to `<`. The closing delimiter is `</`. Standalone
> `@` remains the marker for compiler intrinsics and compiler magic. See
> [ADR 0032](adr/0032-universal-api-project-identity-and-contexts.md).

> **Design direction:** contexts are reserved for domain-specific syntax
> integration, not as general-purpose namespaces for declarations. See
> [§17](17-contexts.md) and
> [ADR 0032](adr/0032-universal-api-project-identity-and-contexts.md).

> **Zith-- distinction:** normal `macro` and `raw macro` are the macro forms implemented in
> Zith--. Zith tags do not change its macro contract or diagnostics.

- Zith-- macros accept a special first parameter named `attributes` (with no meta-type) to
  receive call-site attributes. `@closure|k: 1|(...)` exposes values as `attributes.k` inside
  the macro body. A macro without that parameter rejects attribute syntax.

```zith
macro log(msg: expr) { @println("[LOG] ", msg); }

raw macro swap(a: identifier, b: identifier) {
    let _tmp = a; a = b; b = _tmp;
}

// Default/raw macro with attribute parameter
macro closure(attributes, body1: block) { body1; }
@closure|k: 1|({ ... })

// Zith-- macro parameter meta-types: identifier, expr, condition, block, body
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

The `::` scope-resolution operator remains a separate roadmap item. This
chapter describes only the default and raw macro resolution behaviour.

### 15.1 Zith-- `@`-Prefixed Macro Calls

In Zith--, the `@` prefix distinguishes a macro call from an ordinary function
call:

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

This rule describes Zith-- macro calls only. Full Zith uses standalone `@` for
compiler intrinsics and compiler magic. Its `@<` tag delimiter is a separate
compound delimiter.

### 15.2 Full-Zith Tags

Tags provide domain-specific syntax integration in full Zith. They are not
macros, and their bodies are not implicitly treated as Zith statements. A tag
declaration specifies the representation of its required body argument.

```zith
@<p>Se e louco, Zith full e foda</p>
```

The `@<` opener and `</` closer are dedicated delimiters. A tag declaration
must accept a body argument. Its body kind determines how the compiler presents
the content to the tag:

| Body kind | Contract |
|---|---|
| `tokens` | Exact source text as written, without Zith interpretation. |
| `identifier` | Exactly one identifier. |
| `ast` | Structured syntax, not necessarily evaluated. |
| `block` | Code parsed as a Zith block. |

These body kinds are not exhaustive. The exact tag declaration grammar,
attribute grammar, and empty-body rules remain under design. This is a full-Zith
design decision only. Zith-- behavior and its existing `macro` forms are
unchanged.

---

*[Zith Language Specification](Zith-spec.md) — Draft v0.9*
