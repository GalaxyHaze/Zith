# Interactive Input For The WASM Playground

Status: accepted

This ADR accepts promises `ABI-WASM-INPUT-01` through
`ABI-WASM-INPUT-05` in
[`docs/plans/wasm-playground-interactive-input.md`](../plans/wasm-playground-interactive-input.md).
The normative behavior is specified in
[`docs/specs/wasm-playground-interactive-input.md`](../specs/wasm-playground-interactive-input.md).

The browser playground must let a program using the existing `InputLine` API
receive input from the playground terminal without changing the VM execution
model for other targets. Add generic byte-oriented input/output callbacks at
the VM boundary, and use Emscripten Asyncify only in the WASM build to suspend
execution while the browser terminal waits for a line. Enter submits UTF-8
bytes followed by newline, EOF is distinct from stopping a run, and stopping
returns a dedicated cancellation status rather than EOF. Keep one active run,
do not add a worker, and limit cancellation to runs suspended on input. Grow
`InputLine` storage instead of imposing a fixed line limit. Bump the WASM ABI
to version 3 for the new host input import.
