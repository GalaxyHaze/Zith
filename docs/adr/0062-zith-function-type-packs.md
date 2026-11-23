# Function Type Packs in Zith

Status: accepted design, implementation pending

Zith functions may declare one final generic type pack, written `Ts...`, and
bind their final call arguments to a tuple-valued parameter such as `args: Ts`.
The tail may be empty, and an explicitly passed tuple is one argument in that
tail. `Ts` denotes the collected tuple as a unit, while `...Ts` expands its
types in a generic argument list. Calls may infer the pack or specify it
explicitly, and a constraint on `Ts` applies to each collected type.

Each element follows ordinary ownership rules. A function may use either one
heterogeneous type pack or one homogeneous variadic slice `[...]T`, never both.
Fixed generic parameters precede the type pack, which is final. Type packs are
limited to functions; structs, aliases, enums, unions, and traits do not
declare them. Tuples and comptime provide the needed type-level aggregation for
those declarations without adding a second open-ended arity mechanism.
