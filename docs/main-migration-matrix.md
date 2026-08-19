# Migration Matrix: `main` Features to the Toolkit Architecture

This matrix records how mature behavior from the `main` branch (reference commit
`85312df`) should be ported into `autonom` without importing the parallel
handwritten frontend, sema, symbols, or types architectures. `autonom` remains
the canonical declarative branch: generated lexer, AST, parser, CLI, session,
diagnostics, and config surfaces stay authoritative, and handwritten services
implement behavior behind them.

**Current phase:** parser/lexer/AST migration. The generated surfaces, the
handwritten parser, the `Parsed` session stage, and `zithc check` are in place.
Sema, import resolution, macro expansion, symbols, HIR/SIR lowering, and backend
work remain future migration rows.

Rows are grouped by the toolkit surface that owns the feature. **Generator
change** means the row needs a new declaration shape or a protected shared
tooling edit, which requires explicit user approval before any generator source
is touched. "Status" tracks the incremental port, so later migration work can
use this document as the checklist.

## Rule of Thumb

- Structure and table wiring belong in `.rules`/TOML.
- Behavior belongs in handwritten C++ (`actions.cpp`, `handlers.cpp`,
  `dispatch.cpp`, or a focused service).
- Generated output is build-only. Never edit files under `build/`.
- `src/common/type-system` is the single type-system integration point.
- `src/symbols/` and `src/common/import/` are protected and not migrated here
  without explicit approval.

| # | `main` feature | Toolkit subsystem | Handwritten implementation | `.rules`/TOML changes | Tests | Generator change? | Status |
|---|---|---|---|---|---|---|---|
| 1 | Multi-char operator maximal munch | `frontend/lexer` | `actions.cpp` only if hooks are needed | `lexer.rules` `[tokens] compound` | `tests/frontend/lexer-test.cpp` | No | Completed |
| 2 | Compound assignment and bitwise operators | `frontend/lexer` | none beyond lexer behavior | `lexer.rules` `[tokens] compound`, `[tokens] operators` | extended `lexer-test.cpp` | No | Completed |
| 3 | `->` arrow operator | `frontend/lexer`, `frontend/parser` | `parser/actions.cpp` for type/return hooks | `lexer.rules`, `parser.rules` | extended `parser-test.cpp` | No | Completed |
| 4 | `for`, `when`, ranges, arrays, structs, optionals, casts, null checks | `frontend` grammar and `session` stages | parser/type/sema service behind generated surfaces | `lexer.rules`, `ast.rules`, `parser.rules`, `session.rules` | new frontend/session tests | Possibly, only if a declaration shape is absent | Parser complete; sema/backend future |
| 5 | Generic declarations and aliases | `common/type-system`, `session` TypeChecked | `TypeContext` registry and type inference | `type-system.rules`, `session.rules` | `tests/common/type-system` | No | Not started |
| 6 | Macro expansion | `frontend` after AST ownership/nodes | macro service using stable generated AST | `ast.rules`, `parser.rules` | frontend macro tests | No | Not started |
| 7 | C-interoperability syntax | `frontend`, `common/type-system`, optional C-interop service | type lowering and foreign declarations | `ast.rules`, `parser.rules` | optional c-interop tests | No | Not started |
| 8 | Primitive/composite type interning and stable IDs | `common/type-system` | `TypeContext` by-id lookup and registration | `type-system.rules` | `type-system-test.cpp` | No | Not started |
| 9 | Common-type selection, coercions, casts, overloads | `common/type-system`, `session` TypeChecked | `TypeContext` static+dynamic tables | `type-system.rules` | type-system tests | No | Not started |
| 10 | Ownership/mutability qualifiers | `common/type-system`, `session` | qualifier model in `TypeContext` | `type-system.rules` | type-system tests | No | Not started |
| 11 | Generic parameters and placeholders | `common/type-system` | recursive placeholder/completion on `TypeContext` | `type-system.rules` | type-system tests | No | Not started |
| 12 | Flow-sensitive nullability/narrowing | `session` TypeChecked->NraResolved | handwritten narrowing service | `session.rules` | session tests | No | Not started |
| 13 | Typed AST annotations | generated AST + `session` TypeChecked | annotation map attached to session stage | `ast.rules` optionally | session tests | No | Not started |
| 14 | HIR lowering and verification | `session`, existing `common/sir` | HIR module + verify service | `session.rules` | HIR tests | No | Not started |
| 15 | VM code generation/execution | existing `codegen` VM | VM service behind backend interface | none | codegen tests | No | Not started |
| 16 | Object/native emission | backend interface + existing SIR/codegen | backend adapter, no CLI logic | none | backend tests | No | Not started |
| 17 | ZIRL serialization/deserialization | existing `common/sir/flat` where possible | portable serializer service | none | SIR/cache tests | No | Not started |
| 18 | Persistent cache hydration/invalidation | existing `src/cache` | cache stage implementation | `cache.rules`, `session.rules` | cache tests | No | Not started |
| 19 | LLVM/WASM/platform backends | backend adapter | adapter behind the backend interface | none | optional CI tests | No | Not started |
| 20 | `build`, `run`, `check`, `test`, docs, clean | generated CLI + session | thin handlers calling session stages | `cli.rules` | CLI integration tests | No | Partly wired (build/run/check), rest deliberate stubs |
| 21 | Formatter behavior | dedicated formatter service | formatter visitor over generated AST | `cli.rules` | formatter tests | No | Explicit stub |
| 22 | C header import | separate C-interop service | optional dependency-gated library | none | optional tests | No | Not started |
| 23 | C API and WASM adapters | optional targets | adapter behind stable API | none | optional tests, CI | No | Not started |
| 24 | Shell completion and platform packaging | generated CLI | completion service/handler | `cli.rules` | CLI tests | No | Explicit stub |
| 25 | Diagnostic categories/labels/suggestions/rendering | existing `common/diagnostic` | structured diagnostic service | `diagnostic/error.rules`, session stages | diagnostic tests | No | Partly present |
| 26 | Cache fingerprints/artifact metadata | existing `src/cache` | manifest service using existing cache tables | `cache.rules` | cache tests | No | Not started |
| 27 | Stable AST identity | generated AST + `common/ast` | stable IDs/ownership helpers in generated AST surface | `ast.rules` if needed | AST tests | Possibly, only if a declaration shape is absent | Not started |
| 28 | Source maps/arenas/results/interned strings | existing `common/memory` | use existing types only | none | common tests | No | Already present |

## Porting Order

1. Syntax and lexer waves (rows 1-3) first: they need no generator feature and
   establish the maximal-munch surface.
2. Type integration through `src/common/type-system` (rows 8-11) before any
   HIR/SIR or codegen work, because every later stage consumes type IDs.
3. Session semantic stages (rows 4-13) behind the generated stage list.
4. HIR/SIR and codegen (rows 14-19) once the type layer is stable.
5. Product features and optional adapters (rows 20-24) last.
6. Keep every unsupported feature as a documented stage/CLI stub until its
   implementation and tests are ready.

## Completion Boundary

Rows 1-3 cover lexer maximal munch and parser recognition of the `->` operator.
Row 4 adds parser support for the named control-flow/value forms, but not their
type checking or execution. `~>` is the `when`/`match` case delimiter and belongs
with row 4, so it is intentionally not added to the rows 1-3 lexer compound list
or expression-arrow rules.

A migration phase is complete only when the behavior has a focused test or demo,
the generated/source boundary is unchanged, the relevant session stage has a real
implementation or a deliberate documented stub, diagnostics stay structured and
source-located, the full test suite passes, and no duplicate runtime/type/symbol
subsystem was introduced.

## Phase 1 Close-Out

The current phase is complete with the full suite passing:

- Lexer parity: complete.
- Generated AST parity: complete.
- Parser parity and recovery: complete.
- Session `Parsed` stage: complete.
- `zithc check` reaching `Parsed`: complete.

Future work remains for:

- Sema/type checking and flow-sensitive nullability.
- Import resolution and the import graph.
- Macro expansion and symbols.
- HIR/SIR lowering, cache hydration, VM/codegen, and backends.
- Remaining CLI/service wiring outside `check`.
