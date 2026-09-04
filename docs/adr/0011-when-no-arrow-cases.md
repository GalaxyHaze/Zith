# When Cases Without `~>`

`when` arms stay parenthesized, but the `~>` marker becomes optional and deprecated. A case such as `(Status.Ok) println("ok")` is now the canonical spelling; `(Status.Ok) ~> println("ok")` continues to compile with `W1008 DeprecatedSyntax` and lowers normally.

The first parenthesized island is always a pattern against the subject. Non-boolean operands in that island desugar to equality with the subject, so `(Status.Ok or Status.Error)` means `status == Status.Ok or status == Status.Error`. Any later island separated by top-level `and`/`or`/`xor` is an ordinary boolean condition, so `(Status.Ok) and (retries < 5)` is allowed and recommended over inlining the guard inside the pattern island.

Cases are separated by a mandatory comma (omitted after the final case); after the condition islands, the next token starts the body. `(_)` must remain the sole island of its case and must still be the final case. `~>` is kept as the explicit escape for bodies that would otherwise be ambiguous to read, but is not part of the canonical syntax.
