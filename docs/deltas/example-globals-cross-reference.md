# Globals cross-reference to Const Fields

Status: draft
Target: docs/Zith--.md#globals
Owner: -
Related: -

This is the worked example referenced by [README.md](README.md). It is a real
candidate edit, not a placeholder, so it can be reviewed and imported or
dropped like any other delta.

## Problem

The `Globals` section explains module-level `const` storage and `extern`
bindings, but it never points a reader to the `Const Fields` section, which
covers the other way a value can be fixed at compile time. A reader who wants a
compile-time constant instead of runtime storage has to find that section on
their own.

## Change

### docs/Zith--.md#globals

- add: `Para um valor fixo em tempo de compilação em vez de storage, ver Const Fields.`

## Rationale

The two sections answer adjacent questions: where a value lives at runtime, and
where a value is fixed at compile time. A single cross-reference line closes the
gap without restating either section.
