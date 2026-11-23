# Zith-- Variadic Macros and Caller-Scope Defer

Status: implemented

This spec defines variadic parameters and statement repetition for Zith--
macros, and requires a `defer` emitted by a `raw macro` to register cleanup
with the lexical block containing the macro call. It records the implemented
contract consistent with ADR-0055.

## Problem Statement

Zith-- macros currently require exact arity. A macro cannot generate one
statement for each item in a heterogeneous call-site argument list. A
`raw macro` can emit a `defer`, but cleanup is currently associated with the
macro expansion boundary instead of the lexical block containing the call.
This prevents macros from registering caller-scoped cleanup for a variable
number of resources.

## Solution

Normal and raw macros can declare one final variadic parameter. The parameter
captures zero or more arguments of its declared macro argument kind.
Postfix `...` on an expression statement repeats that statement once for each
captured argument, in argument order. A `defer` emitted by a `raw macro`
registers on the lexical block containing that macro invocation. Repeated
statements inside the deferred body run in input order, while distinct defer
registrations keep the existing reverse registration order.

## Requirements

1. A macro declaration may contain at most one variadic parameter.
2. A variadic parameter must be the final parameter in the macro declaration.
3. A variadic parameter declares one Zith-- macro argument kind: `val`,
   `identifier`, or `ast`. A `val` argument uses normal call syntax without
   `=`, an `ast` argument requires `=expr`, and an `identifier` argument is
   one unqualified name without `=`.
4. The variadic parameter captures the ordered tail after all fixed parameters.
   The tail may contain zero arguments.
5. A macro invocation must provide every fixed argument. The compiler must
   report a diagnostic when it provides fewer arguments than the fixed
   parameters require.
6. The compiler must validate each captured argument against the variadic
   parameter's declared kind and report a diagnostic for an invalid argument.
7. Postfix `...` may follow an expression statement in a macro template to
   repeat that statement for the variadic parameter.
8. The repeated expression statement is instantiated once per captured
   argument, preserving the call-site argument order. An empty tail emits no
   repeated statements.
9. A variadic parameter may be referenced only by a repeated expression
   statement. The compiler must diagnose a variadic parameter used in any
   other template position, including an expression statement without the
   repetition marker.
10. A repeated statement substitutes the current captured argument everywhere
    the variadic parameter occurs in that statement. Other macro parameters
    and template structure retain their existing expansion behavior.
11. The repetition marker is compile-time template behavior. It does not emit
    a runtime loop or change function variadic-slice forwarding or pack
    spreading.
12. A `raw macro` expansion does not create a lexical cleanup scope around its
    emitted statements. A `defer` emitted by the expansion registers with the
    lexical block containing the invocation.
13. The generated cleanup runs when that containing block exits, including
    ordinary fallthrough and applicable control-flow exits. It must not run
    merely because macro expansion finished or before the next statement in
    the containing block.
14. A repeated expression statement inside a `defer` body runs once per
    captured argument and in input order when the cleanup executes.
15. An empty variadic tail emits no repeated cleanup statements. If the
    repeated statement is the sole content of a generated defer body, the
    expansion registers no cleanup.
16. Distinct defer statements retain existing LIFO registration order.
    Repetition changes only the order of statements within one generated
    cleanup body.
17. A normal macro retains its existing hygiene behavior. A raw macro retains
    its existing literal, call-site name resolution behavior.
18. Names emitted by a raw macro, including names substituted from
    `identifier` arguments, resolve under the existing call-site scope rules.
19. When a generated cleanup refers to an owner, existing explicit ownership
    rules apply. The owner remains usable until the cleanup runs, and cannot
    be moved, rebound, or explicitly destroyed before then.

## User Stories

1. As a macro author, I want a final variadic parameter to capture zero or more
   arguments, so that one macro can handle calls with different arities.
2. As a macro author, I want to select the argument kind for the variadic
   parameter, so that each captured argument follows the same validation
   contract as a fixed macro parameter.
3. As a macro author, I want to combine fixed parameters with a variadic tail,
   so that a repeated template statement can use shared configuration and a
   different argument on each repetition.
4. As a macro author, I want a repeated expression statement to substitute
   each captured argument in order, so that generated operations follow the
   order written by the caller.
5. As a macro author, I want an empty variadic tail to produce no repeated
   statements, so that zero-item calls remain valid and do not create
   accidental cleanup.
6. As a macro author, I want invalid variadic declarations and argument kinds
   to produce diagnostics, so that malformed macro contracts are easy to fix.
7. As a macro author, I want the compiler to reject a variadic parameter used
   outside a repeated expression statement, so that every use has an
   unambiguous substitution rule.
8. As a user of a normal macro, I want variadic repetition to preserve normal
   macro hygiene, so that adding a variadic tail does not introduce accidental
   name capture.
9. As a user of a raw macro, I want emitted names and identifier arguments to
   retain call-site resolution, so that a macro can refer to locals in the
   block where it is invoked.
10. As a user of a raw macro, I want an emitted `defer` to belong to the block
    containing the invocation, so that cleanup happens when that block exits.
11. As a user of a raw macro, I want caller-scoped cleanup to remain active
    across later statements and control flow, so that expansion does not
    shorten the resource lifetime.
12. As a user of a variadic cleanup macro, I want all generated operations in
    one deferred body to run in argument order, so that cleanup order is
    predictable.
13. As a user of multiple defer statements, I want their existing LIFO order
    to remain unchanged, so that this macro feature does not alter defer
    semantics elsewhere.
14. As an owner of a resource registered for generated cleanup, I want existing
    ownership checks to prevent moving, rebinding, or explicitly destroying
    it before cleanup, so that the registered cleanup remains valid.
15. As a user of function variadic slices or pack spread, I want those separate
    mechanisms to keep their current syntax and behavior, so that macro
    repetition does not conflate compile-time syntax expansion with call
    argument forwarding.

## Implementation Decisions

- Support one final variadic parameter on both normal and raw macros.
- Reuse the existing `val`, `identifier`, and `ast` macro argument kinds.
- Permit zero captured arguments and validate every captured argument
  independently.
- Restrict postfix template repetition to expression statements for this
  feature.
- Require every use of a variadic parameter to occur in a repeated expression
  statement.
- Expand each repeated statement once per captured argument in source order.
- Treat statements emitted by a raw macro as belonging to the invocation's
  lexical block, including defer registration. Do not add an expansion-boundary
  scope.
- Preserve existing normal-macro hygiene, raw-macro name resolution, defer
  cleanup order, and ownership analysis.

## Testing Decisions

- Test observable behavior through a compiler end-to-end seam that compiles,
  links, and runs source using normal and raw variadic macros.
- Verify multiple repeated arguments execute in source order and that an empty
  tail emits no operation.
- Verify raw-macro cleanup runs at the caller block's exit, not at the macro
  expansion boundary, including a nested lexical block.
- Verify repeated operations within one deferred body use argument order while
  separate defers preserve LIFO order.
- Verify diagnostics for a non-final or repeated variadic declaration, a
  missing fixed argument, a wrong-kind captured argument, and use of a
  variadic parameter outside a repeated expression statement.
- Verify existing normal-macro hygiene and raw-macro call-site lookup remain
  unchanged.
- Verify generated cleanup remains subject to the existing ownership checks.
- Follow the existing macro expansion and defer end-to-end tests. Assert
  compiler diagnostics or program output, not AST or expansion internals.

## Out of Scope

- Function variadic parameter or variadic-slice semantics.
- `...slice` forwarding and general pack spread.
- Repetition of statements other than expression statements.
- Repetition over arbitrary AST collections or a runtime loop construct.
- Expression arguments for `@deferDestroy`, which accepts identifiers.
- Changes to cleanup order between separate defer registrations.
- Automatic cleanup or a language-level `drop` feature.
- Thread-specific behavior or changes to the `soon/thread` API.

## Further Notes

The motivating use is a raw cleanup macro such as
`@deferDestroy(vec, arena)`, which registers one deferred body in the caller's
block and invokes `vec.destroy()` followed by `arena.destroy()` when that block
exits. The ownership contract is recorded in ADR-0055. The implementation
record is in the variadic macro plan.
