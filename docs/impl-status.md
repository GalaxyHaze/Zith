# Implementation Status: `autonom` vs `main`

Reference branch: `main` at commit `85312df`
Current tracking branch: `autonom`
Latest implementation commit on `autonom`: `14cac7a`

This document records how much of the committed `main` behavior is currently
implemented in the declarative-toolkit architecture on `autonom`. It is meant
to be updated whenever the session stage coverage, parser surface, or semantic
error catalogue changes.

## Pipeline Status

| Stage | Status | Notes |
|---|---|---|
| `Source` | Implemented | session ownership of source text and spans |
| `Lexed` | Implemented | generated lexer over the declarative `lexer.rules` tables |
| `Parsed` | Implemented | generated AST/parser with handwritten parser actions |
| `Scanned` | Implemented | `toolkit::resolution::scanProgram` records consultable `ScanInfo` |
| `Imported` | Implemented | `toolkit::resolution::importProgram` loads imports and finalizes the import graph |
| `Resolved` | Implemented | `toolkit::resolution::resolveModules` stores `ResolvedInfo` with module-scoped visibility |
| `TypeChecked` | Implemented | handwritten `src/sema` runs behind the generated session stage |
| `Solved` | Stub | solver pass not implemented |
| `NraResolved` | Stub | NRA/ownership proof pass defers to the future |
| `HirLowered` | Stub | HIR lowering not implemented |
| `CodegenReady` | Stub | backend selection exists, module lowering is future work |
| `Cached` | Stub | cache hydration/invalidation is future work |

## Parsed Surface

The current grammar accepts and represents the following parsed forms without
requiring a portable `main`-style handwritten frontend:

- declarations: functions, variables/globals, structs, enums, unions,
  type aliases, traits/interfaces, states, macros, imports, words, and
  contexts
- function kinds: standard, `extern`, `const`, and `raw` functions
- function parameters with optional default values and memory-qualified types
- visibility ranges (`pub`, `pub(..)`, `pub(0..)`, `pub(0..=)`,
  `pub(=..0)`, `pub(=..=)`, siblings/neighbors) and module/asset/header import
  metadata
- type expressions: names, pointers, optional types, slices, arrays, function
  types, and `raw opaque`
- expressions: literals, names, unary/binary operators, assignment, calls,
  indexing, field/arrow access, optional propagation, casts, struct literals,
  array literals, blocks, `if`, `for`, `when`, ranges, layout
  intrinsics, null checks, macro calls, and placeholders
- statements: expressions, bindings, returns, breaks/continues, `enter`,
  `leave`, and `jump`

Legacy `marker`, `dock`, `stackful`, and `while` forms are rejected with
diagnostics and no longer produce old flow nodes.

Because `TypeChecked` is the implemented semantic boundary, anything below that
line is intentionally deferred and should be listed under "Deferred Work"
instead of being treated as a silent success.

## Resolution Coverage

`Stage::Scanned` records top-level declarations and rejects duplicate
non-overloadable declarations before later phases run. `Stage::Imported`
loads normal imports recursively relative to the project root, names imported
modules deterministically, records parent-to-child import graph edges, and
finalizes the graph so circular imports fail. Header/asset/export imports are
accepted at `Imported` and preserved in `ImportInfo`; `Stage::Resolved`
rejects unsupported header/asset imports before exposing symbols.

`Stage::Resolved` stores per-module `ResolvedSymbol` entries in `ResolvedInfo`.
Symbol visibility follows the parser-encoded module ranges:

- default/private declarations are visible only inside their own module
- bare `pub` and `pub(..)` are globally visible
- `pub(0..)` and `pub(0..=)` place compatible bounds on ancestor/descendant
  and same-parent visibility rules
- `pub(=..0)` and `pub(=..=)` implement direct-parent/direct-child visibility
- `from ... {}` selectors and aliases expose selected names without leaking
  unselected originals

Resolution ordering is first-wins: local symbols, then visible
dependencies/same-parent siblings, then `from` selectors and aliases.

## Semantic Coverage

`Stage::TypeChecked` runs over the generated AST and stores consultable
annotations through `toolkit::sema::TypeCheckedInfo` on the session context.
The semantic checker currently handles:

- primitive and named type registration
- function declaration lowering and per-declaration overload entries
- function calls, arity checks, argument coercion, overload selection, and
  ambiguous-call detection
- aliases, structs, enums, unions, traits/interfaces, generic placeholders,
  arrays, slices, optionals, and pointers
- literal classification and integer/float adaptation
- arithmetic, comparison, shift/bitwise, logical, unary, assignment, pointer,
  optional, indexing, field, arrow, cast, struct-literal, and array-literal
  typing
- `if`, `for`, `when`, and range validation
- `null` and `is null` checks requiring optional types
- layout intrinsics such as `@sizeOf` and `@offsetOf`
- memory qualifiers (`unique`, `share`, `lend`, `view`, `belong`, `mut`) on
  bindings, parameters, declarations, and place expressions
- state declarations plus deferred `enter`/`leave`/`jump` flow semantics
- unsupported syntax rejection with a structured semantic code

The checker is hand-written and intentionally stays on this branch's generated
AST, session, and type-system surfaces. It does not copy `main`'s
`src/sema/*`, `frontend`, `hir`, `session`, `types`, `symbols`, or `cinterop`
directories.

## Error Catalogue

New semantic diagnostics are declared in `src/diagnostic/error.rules` and use
stable numeric codes plus structured messages. Tests assert on these codes
rather than on legacy `main` message strings.

| Code | Name | Example message |
|---|---|---|
| `E2001` | `UndefinedIdent` | `unknown identifier 'y'` |
| `E2002` | `DuplicateDecl` | `duplicate function signature 'main'` |
| `E2006` | `NoMember` | `unknown field 'missing' on type 'Point'` |
| `E2007` | `NoMatchingFn` | `function call arity mismatch` |
| `E2008` | `AmbiguousCall` | `call is ambiguous between several overloads` |
| `E2010` | `UnsupportedSyntax` | `macro calls are not supported in semantic analysis` |
| `E3001` | `TypeMismatch` | `return type does not match declared return type: expected 'i32', has type 'bool'` |
| `E3002` | `CannotInfer` | `cannot infer type of 'x'; assign a value before reading it or add a type annotation` |
| `E3003` | `InvalidCast` | `'as' supports numeric conversions and 'raw opaque' pointer conversions` |
| `E4004` | `WriteThroughView` | `cannot write through 'v': a 'view' binding is read-only` |

The message text above is the diagnostic `message` field. The generated
renderer can enrich it further with the catalogue `template`, source span, and
`note:` field when displayed.

## Examples

Accepted program:

```zith
struct Point { x: i32, y: i32 }

fn add(p: Point, q: Point): i32 {
    return p.x + q.y;
}

fn main(): i32 {
    let a: Point = Point { x: 1, y: 2 };
    return add(a, a);
}
```

Rejected because a `view` binding is read-only:

```zith
fn read(p: view *i32): i32 { return 0; }

fn main(): i32 {
    let x: mut i32 = 1;
    let v: view *i32 = &x;
    *v = 2;        // E4004: cannot write through 'v': a 'view' binding is read-only
    return read(v);
}
```

Rejected because integer literal overload selection is ambiguous:

```zith
fn h(x: i32): i32 { return 1; }
fn h(x: u32): u32 { return 2; }

fn main(): u32 { return h(1); }
```

Rejected because the syntax is parsed but not yet semantically supported:

```zith
state Ready(x: i32) {}

fn main() {
    enter Ready(1);
}
```

## Deferred Work

- NRA ownership proofs and flow-sensitive nullability refinement
- HIR/SIR lowering and verification
- state/enter/leave/jump execution semantics beyond parse, signature, and
  deferred unsupported checks
- solver passes, codegen, VM/backend lowering, and cache hydration
- cross-module type checking, header/asset import loading, and export
  re-export semantics
- modifying the protected `src/common/import` or `src/symbols` implementations

The current milestone intentionally stops at `TypeChecked` type checking for a
single file; later compiler stages remain session stubs until they have a
matching handwritten service.
