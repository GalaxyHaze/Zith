# Type-Aware Macro Branches And Static Assertions

Status: drawing

Zith-- macros can currently receive syntax-oriented arguments, but cannot
reliably select generated code from the resolved category of a type. Generic
code has the same gap: it cannot discard a branch based on whether a resolved
type is a struct, union, enum, or primitive. This drawing proposes a narrow
type-aware extension to macros and generic code, plus a comptime assertion
built-in. It does not claim that any proposed behavior is implemented.

Size: one Zith-- language subsystem spanning macro argument parsing and
expansion, type-category analysis, generic instantiation, and diagnostics.

## Current Facts

- The documented macro argument meta-types are `identifier`, `expr`,
  `condition`, `block`, and `body`.
- Runtime type tests such as `value is ConcreteType` remain part of the
  existing expression surface.
- Macro expansion currently precedes semantic analysis. Generic instantiation
  currently follows semantic analysis, so type-dependent macro expansion and
  branch pruning require a deliberate stage boundary.
- `docs/Zith--.md` only treats behavior as part of the language contract after
  implementation and verification.

## Agreed Direction

- Add `type` as a macro argument meta-type. It accepts type expressions, not
  only bare names, and interprets the supplied argument as a type.
- Support type-category predicates `T is struct`, `T is union`, `T is enum`,
  and `T is primitive` in ordinary generic code. They accept any resolved
  type. They are compile-time predicates, not runtime value checks.
- Resolve aliases transparently for category predicates. Classify components
  as structs, and preserve the category of nominal types.
- Define `primitive` to cover scalar types. It excludes pointers, including
  `*char` strings.
- Preserve runtime tests such as `value is ConcreteType`. A value tested with
  a category predicate, such as `value is struct`, is invalid.
- Evaluate type-category predicates and boolean combinations in generic code
  for each concrete instantiation. Do not semantically analyze a discarded
  branch.
- Defer expansion of a macro that depends on generic type information until
  the relevant concrete instantiation.
- Do not add `T is type`. The `type` meta-type is a macro parameter category,
  not a general generic constraint.
- Add `@staticAssert` as a language built-in, not as a macro. It takes a
  comptime-evaluable condition and an optional string-literal message. It is
  an instruction with no value. A failure reports the condition and the
  concrete type of the generic instantiation.

## Non-Goals

- Does not add general comptime evaluation, reflection, type construction, or
  type mutation to Zith--.
- Does not add generic constraints or make `T is type` a predicate.
- Does not change the semantics of existing runtime type tests.
- Does not make `@staticAssert` produce a value or accept a computed message.
- Does not implement the proposal or add it to the shipped Zith-- contract.

## Open Decisions

- Which parser representation should carry a type expression through macro
  argument binding without prematurely resolving it.
- Which compiler stage can expand type-dependent macros after generic
  substitution while still allowing their output to enter semantic analysis.
- How branch pruning can happen before semantic analysis of the discarded
  branch without changing the current generic-instantiation contract.
- Which diagnostics and source spans should identify invalid category tests,
  non-comptime assertions, and failed assertions.
- How assertion diagnostics should format nested generic instantiations while
  keeping the condition and concrete type legible.
