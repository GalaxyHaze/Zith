# NRA Surface Spellings

Status: accepted as a full-Zith design direction. Nothing in this decision is
implemented or changes Zith--.

## Context

The NRA surface has drifted across drafts. ADR-0033 fixed the reference model
(`&`, `&mut`, `^`, `%`) but called `^` "bind (formerly `belong`)" and left the
keyword spellings implicit. ADR-0026 fixed the revocable spelling as `'own`
and stated that `'lend` and `'view` do not exist.

This ADR fixes the spellings once so the spec, the ADRs, and the implementation
notes stop contradicting each other. It supersedes the "Source Spelling"
section of ADR-0026 and completes the surface forms of ADR-0033.

## Decision

Each form has a sigil spelling and, where one exists, an equivalent keyword
spelling. The two spellings are the same form and may be used
interchangeably.

| Sigil | Keyword | Meaning |
|---|---|---|
| `&T` | `view T` | read reference |
| `&mut T` | `lend T` | write reference |
| `^T` | (none) | read bind |
| `^mut T` | `bind T` | write bind |
| `%T` | (none) | immutable own |
| `%mut T` | `own T` | mutable own |

Only `T` (a plain binding) has no qualifier and no keyword. The read forms
`^T` and `%T` have no keyword. `belong` is the old name of the write bind and
is removed; the current keyword is `bind`. `unique` is the old name of `own`
and is removed. `default` describes the plain `T` case and is not a keyword.

The revocable prefix is the sigil `'` or the long spelling `grant`. It
combines with every form except a bare `T`, so `'&T`, `'&mut T`, `'^T`,
`'^mut T`, `'%T`, and `'%mut T` all exist. `'%mut T` equals `grant %mut T`.
`'T` does not exist. The prefix appears only where the argument or type is
declared, not at the call site.

## Consequences

- The spec section 4 lists both spellings of each form.
- ADR-0033 examples such as `Option<own Self>` and `Option<bind Self>` are
  current: they are the keyword spellings of `%mut` and `^mut`.
- ADR-0026's "Source Spelling" section is superseded: the revocable prefix
  is not restricted to `own`, it applies to every qualifier except a bare
  `T`.
- Zith-- is unaffected. It implements only the `lend`/`view` call-annotation
  slice (SRA) and rejects the other qualifiers. The lexical disambiguation
  below is a Zith-only change and must not touch the Zith-- scanner.

## Considered Options

Making the keywords legacy-only and the sigils canonical was rejected: both
spellings coexist and are equivalent, so the choice is stylistic. The read
bind `^T` and the immutable own `%T` simply have no keyword.

> Superseded by [ADR-0056](0056-nra-erased-forms-and-spelling.md): the choice
> is no longer stylistic. The canonical spelling follows mutability, the
> immutable forms use the sigil, the mutable forms use the keyword, and the
> formatter rewrites the aliases. ADR-0056 also settles that an erased outer
> type (`opaque`, `dyn T`) is not ownable.

## Lexical Note

The revocable sigil `'` shares its opening character with the character
literal. The disambiguation rule (one character or one escape between `'`
and `'` is a literal, anything else opens a revocable qualifier) is part of
the Zith surface only. The Zith-- scanner in
`src/frontend/ast-lowerer.cpp` currently accepts a multi-character literal
body, and it must stay that way for Zith--. Tightening character literals is
therefore a Zith-only change and is out of scope for the Zith-- slice.
