# General Pack Spread in Zith--

Status: accepted design, implementation partial

Zith-- uses `...pack` to expand a value pack into positional call arguments.
This is value-level argument expansion, not generic type-pack expansion.

## Decision

### Argument-list expansion

- In a call argument list, `...pack` contributes one positional argument for
  each member of the pack, in member order. The pack's arity is known when the
  call is type-checked.
- A pack spread may appear before, between, or after ordinary arguments.
  Multiple spreads may appear in one call. The resulting argument sequence is
  the written sequence with each spread replaced by its members.
- An empty pack contributes no arguments. A member that is itself a pack is
  passed as one argument and is not recursively expanded.
- The spread operand must have a pack type. A non-pack operand is invalid
  except where the call uses the explicit final-slice forwarding form specified
  by [ADR-0060](0060-explicit-variadic-slice-forwarding.md).
- Pack member names do not affect positional expansion. Named packs expand in
  their declared member order. A caller that needs named local bindings must
  destructure the pack explicitly.
- Each spread operand is evaluated once. The call checks and resolves the
  argument sequence after expansion, using the resulting arity and each
  member's type for overload selection and parameter matching.

### Variadic parameters and C calls

- In a language variadic-slice call, each expanded member is one tail element.
  The existing collection and element-conversion rules apply to those
  elements.
- `...slice` and `...array` retain their distinct ADR-0060 meaning: they
  forward one slice or array as the complete final variadic-slice tail. They
  do not become heterogeneous pack spreads.
- In a C variadic call, each expanded member enters the variadic tail as an
  individual argument and receives the same default argument promotions and
  ABI treatment as an explicitly written argument of that type.
- The rules apply to direct and overloaded calls, method calls, and `dock` and
  `jump` calls. Each form resolves and validates the expanded positional
  sequence against its target.

### Pipelines and ownership

- In a pipeline, `it` denotes the complete left-hand value. When it is a pack,
  `...it` expands its members in a call argument list under these rules.
- In a consuming `|>` stage, expanded members follow ordinary ownership rules.
  The stage must consume `it` exactly once on each execution path.
- In an observing `also` stage, `...it` may bind only to read-only `view`
  parameters. Those borrows cannot escape the stage.
- Pipeline ownership, use-count, and escape rules are specified in
  [ADR-0055](0055-zith-minusminus-explicit-ownership-and-pipelines.md).

## Separation from Zith Generic Type Packs

- This ADR defines value-pack expansion in Zith-- call argument lists. It does
  not define generic declarations, generic argument lists, type inference, or
  type-level expansion.
- Zith function type packs are a separate feature. `Ts...` declares a generic
  type pack, `...Ts` expands its types in a generic argument list, and a
  parameter such as `args: Ts` represents the collected call arguments as a
  tuple. Those rules belong to Zith, not Zith--, and are outside this ADR.
- Similar spelling does not make the features interchangeable. In particular,
  this ADR does not make a value expression valid as a generic type pack, or a
  generic type pack valid as a value argument.

## Non-Goals

- This ADR does not add general iteration over packs or spread in contexts
  other than call argument lists.
- It does not define pack construction from another pack, element-wise pack
  operations, membership, or implicit introduction of member names.
- It does not change explicit slice forwarding in ADR-0060, macro repetition,
  or Zith generic type packs.
