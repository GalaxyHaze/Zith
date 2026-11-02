---
id: guide-contexts-words-macros
title: Contexts, Words & Macros
section: Language Guide
output: zith--/guide/D-contexts-words-macros.html
aliases: language/D-contexts.html
kind: editorial
---
# Contexts, Words & Macros

Normal macros and `raw macro` are the macro forms available in Zith--. Zith
defines a separate design for contexts, words, and tag items; these are not
part of the Zith-- language contract.

## Macros

Declare a macro with `macro name(param: meta_type) { body }` and call it with the `@` prefix. Parameter meta-types are `identifier`, `expr`, `condition`, `block`, and `body`.

```zith
macro twice(v: expr) {
    v + v
}

fn use_macro(v: i32): i32 {
    @twice(v)
}
```

A normal macro is hygienic: bindings introduced by the template are renamed, so they cannot capture or be captured by names at the call site. Every other name in the template resolves through the call-site scope, with globals and imports visible when they are not shadowed. A macro body is not analysed as code before expansion; resolution is keyed by node identity, not by source text.

`raw macro` opts out of hygiene. Its body splices literally into the call site, and names resolve there first and only then fall back to the module and global scope. Use it when the macro is meant to touch the caller's bindings.

```zith
raw macro increment(name: identifier) {
    name = name + 1;
}

fn main(): i32 {
    var count: i32 = 0;
    @increment(count);
    count
}
```

Prefer a normal macro unless the macro must deliberately access or modify a
binding from the call site. A `raw macro` is powerful because it gives up the
normal hygiene boundary.

## Tag macros

A `tag macro` is invoked with an HTML-like statement form. It takes a `body` parameter and, optionally, the special `attributes` parameter, which exposes named attributes as `attributes.name`. The declaration itself is reported with `E2010` in `Zith--`; this syntax is not part of the Zith-- contract.

```zith
tag macro RunTwice(attributes, body: body) {
    let title = attributes.title;
    body;
    body;
}

fn main(): i32 {
    var total = 0;
    <RunTwice title: 1>
        total = total + 1;
    </RunTwice>
    total
}
```

The parser still recognises tag macros as statement-position only, with dedicated diagnostics for misuse: `E2017` for a tag macro used as a value, `E2018` for a mismatched closing tag, `E2019` for an unknown attribute, and `E2020` for attributes passed to a macro that does not accept them. The declaration rejection `E2010` is the current boundary in `Zith--`. The general macro codes `E2011`-`E2016` cover unknown macros, arity, argument kind, recursion, duplicates, and `raw macro` used as a value.

## Contexts and words

`context` blocks, `use` statements, and the `prefix`, `suffix`, `infix`, and `nop` word declarations are accepted as declarations and then skipped entirely: the body is consumed without semantics. Word call and word sequence expressions have no parser support.

Read the [Contexts](doc:reference-17-contexts), [Words](doc:reference-16-words), and [Macros](doc:reference-15-macros) chapters for the separate Zith design, and [Implementation Status](doc:reference-implementation-status) for the Zith-- compiler boundary.
