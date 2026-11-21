# Raw Call Marker in Zith--

Status: accepted for Zith-- only. Every direct or method call to a `raw fn`
must use an explicit marker, such as `raw f(args)` or
`raw receiver.method(args)`. The marker covers only the invocation, its
arguments remain ordinary expressions, and the function body remains checked.
A missing marker is an error (`E2030`), and `#[raw("reason")]` optionally adds
a non-empty note to that diagnostic. Raw functions cannot yet be used as
function values.

This keeps the raw boundary visible where control crosses it instead of hiding
it in an unchecked body. It supersedes only ADR-0043's statement that Zith--
keeps `raw fn` unchanged. The Full-Zith safety hierarchy is unaffected, and
the `Trust` capability does not waive the call-site marker.

C-header imports require the `raw` marker and emit `W2031` at each call site
because the compiler cannot guarantee the external function's behavioral
boundary. The warning remains even with the `raw` marker. `#[safe]` on a
standard external-symbol alias suppresses both the marker requirement and the
warning for that alias. `extern raw fn` and `raw state` remain deferred.
