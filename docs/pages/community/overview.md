---
id: community-overview
title: Community
section: Community
output: community/D-overview.html
aliases: community/D-overview.html, community/D-chat.html, community/D-contributing.html, community/D-code-of-conduct.html
kind: editorial
---
# Community

Use the Zith issue tracker for Zith-- compiler bugs and language discussions.
Report the compiler version, command line, source sample, expected behaviour,
and actual diagnostics. For language proposals, identify whether the proposal
concerns Zith or Zith-- because their contracts are separate.

## Reporting a compiler issue

Include the smallest source file that reproduces the problem:

```text
zithc --version
zithc check --include stdlib repro.zith
```

Then describe:

- what the compiler accepted or rejected;
- what behaviour you expected;
- the complete diagnostic, including its error code;
- whether the issue appears with `check`, `build`, `run`, or a generated
  target.

Avoid attaching a whole project when a short declaration or expression is
enough. A focused reproducer is easier to test against parser, sema, HIR, and
codegen changes.

## Contributing documentation

Documentation contributions should distinguish Zith design text, the Zith--
language contract, and observed compiler behavior. Canonical specifications
and compiler status belong in `../Zith/docs`; site editorial content belongs
in this repository's `docs/pages`.

Write practical pages in English, lead with the task a reader wants to
complete, and mark unsupported or partial features explicitly. Do not turn an
implementation note into a language guarantee.

## Where to discuss a change

Use the issue tracker for reproducible bugs and proposed language changes.
Use the project chat for questions that need discussion before they become a
specification or implementation issue. Include links to the relevant
`Zith--` guide or Zith reference page when the terminology may be ambiguous.
