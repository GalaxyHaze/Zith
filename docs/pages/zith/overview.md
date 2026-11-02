---
id: zith-overview
title: Zith Overview
section: Zith
output: zith/D-overview.html
aliases: language/D-zith.html
kind: editorial
---
# Zith Overview

Zith is a systems language with its own language-design specification.
Zith-- is a sibling language with a separate specification and compiler
contract. The `zithc` compiler currently implements Zith--; that does not make
Zith-- a subset or an implementation stage of Zith.

## Which documentation should you read?

Read the [Zith-- documentation](doc:zith-subset-overview) when writing code
for `zithc`. Its reference and implementation-status page describe the
language and behavior that the current compiler supports.

Read the [Zith Language Reference](doc:reference-specification) to study the
Zith language design. It is not a description of `zithc` behavior or a plan for
features to be added to Zith--.

The [Implementation Status](doc:reference-implementation-status) page reports
compiler support for Zith-- only. It makes no claim about a Zith
implementation.

## How the languages relate

The website documents two separate languages:

| Language | Audience | Source of truth |
| --- | --- | --- |
| Zith | Readers studying the Zith language design | `docs/Zith-spec.md` and its chapters |
| Zith-- | People writing programs with `zithc` | `docs/Zith--.md`, implementation status, and the practical guide |

The specifications define separate contracts. A feature in the Zith reference
is not thereby available in Zith-- or planned for it. Practical Zith-- pages
use the implementation-status reference to distinguish working behavior from
unsupported or partial behavior.

## Writing programs with `zithc`

Use the [Zith-- overview](doc:zith-subset-overview) and its [Language
Guide](doc:guide-overview) for task-oriented examples. Check
[Implementation Status](doc:reference-implementation-status) for compiler
support. The Zith reference documents a separate language design, not a
Zith-- feature roadmap.
