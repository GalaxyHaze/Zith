# WASM Playground Interactive Input

Status: accepted

The browser playground must let programs use the existing `std/io/console`
input API through its terminal. The VM keeps a synchronous, platform-neutral
I/O contract. Only the WebAssembly host boundary may suspend while it waits
for browser input.

## Requirements

1. A guest program must use the existing `input()` and `InputLine` API.
   Interactive input must not add a WebAssembly-specific standard-library API.
2. `InputLine` must grow to hold the complete line. It must omit the newline
   terminator. If EOF follows bytes without a newline, it must return those
   bytes as one final line, then report EOF on the next read.
3. VM v2 must accept generic byte-input and streaming-output callbacks. Input
   values `0..255` represent bytes, `-1` represents EOF, and `-2` represents
   cancellation. EOF must not cancel a run. Cancellation must produce a
   distinct run status. Streaming output must be delivered before a later
   input read, and the existing accumulated output result must remain
   available.
4. The VM run loop must remain synchronous and must not depend on WebAssembly
   or Asyncify. A run without input callbacks must retain deterministic
   behavior.
5. The WebAssembly host must provide the byte-input import through
   Emscripten-generated Asyncify glue. The execution ABI must be version 3.
   The existing `zith.host_write` import must remain compatible. Flat HIR
   remains version 7.
6. While a guest run waits for input, the browser terminal must submit the
   entered text as UTF-8 bytes followed by one newline byte. It must route
   terminal input to the guest instead of interpreting it as a playground
   command.
7. The playground must allow at most one active guest run. EOF and Stop must
   be distinct actions. Stop must cancel only a run suspended while waiting
   for input. Synchronous guest computation may block the browser thread and
   is not interruptible by this contract.

## Exclusions

This contract does not add an asynchronous VM, a worker, concurrent guest
runs, general computation preemption, or a new language-level input API.
