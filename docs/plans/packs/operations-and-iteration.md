# Pack Operations and Iteration

Status: drawing

Zith has heterogeneous packs and accepted designs for argument-list and
pipeline pack spreading, but no recorded contract for element-wise pack
operations or pack iteration. This drawing collects proposed extensions while
keeping a pack value distinct from its expanded elements and keeping runtime
iteration distinct from comptime expansion.

Size: system, a cross-cutting language feature spanning expression operations
and loop semantics.

Non-goals:

- Does not change the accepted argument-list and pipeline spread design in
  [ADR-0061](../../adr/0061-zith-minusminus-pack-spread.md).
- Does not change function type packs in
  [ADR-0062](../../adr/0062-zith-function-type-packs.md).
- Does not merge membership (`contains`) with iteration (`next`), as recorded
  in [ADR-0012](../../adr/0012-range-in-contains-iterator.md).
- Does not commit to an implementation order or change a language
  specification.

Open decisions:

- Element-wise operations may apply corresponding operations to packs of
  matching arity and produce a pack, as in
  `|x, y| + |x2, y2|`. A scalar may broadcast across a pack, as in
  `|x, y, z| += 5`. The operator set, arity diagnostics, evaluation order,
  mutability requirements, and overload rules remain open.
- Consider additional `...pack` contexts: constructing a pack as `|...pack|`,
  overload resolution when a generic `T` is still open, and
  `if (...pack in container)`. The exact membership semantics of the spread
  form remain open. Without spread, `if (pack in container)` checks membership
  of the pack as one value.
- A runtime `for` does not use `|...|` as its iteration source or binding
  pattern. One candidate is `for ([x, y] in iterator)`, which destructures each
  pack yielded by the iterator. Confirm the accepted pattern and its type rules.
- Direct iteration such as `for (x in |a, b, c|)` is only a comptime candidate,
  not runtime iteration. Each expansion could type-check the body with that
  member's type. The syntax, control-flow rules, and required comptime context
  remain open.
- The initial proposal places pack operations and spread in Zith--, while
  direct pack iteration likely belongs to full Zith. Reconcile this boundary
  with the roadmap's existing full-Zith classification before changing a spec.
