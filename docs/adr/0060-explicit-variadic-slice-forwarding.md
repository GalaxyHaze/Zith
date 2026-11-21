# Explicit Variadic Slice Forwarding in Zith--

Status: accepted and implemented for Zith--.

A call must write `f(...values)` to forward a `[]T` or `[N]T` as the complete
tail of a final `[...]T` parameter. Without `...`, each written argument is
checked as one tail element and collected as before. The marker applies only
to one final variadic-slice argument, including methods and `dock`/`jump`;
`..` remains the pipeline placeholder, and C variadic calls are unchanged.

The previous type-and-position heuristic made a slice-valued tail element
indistinguishable from a slice intended as the complete tail, and made
overload viability depend on inferred candidate shapes. Requiring the marker
makes the caller's intent explicit, lets an unmarked `[]T` be a genuine
element of `[...][]T`, and gives overload resolution a stable,
candidate-specific call plan. `...` does not introduce a general iterable or
heterogeneous spread operation.
