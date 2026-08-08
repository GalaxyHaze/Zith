# WASM Playground ABI

This document describes the stable ABI exported by `zith-playground.wasm` for the browser
playground. The playground performs lexing, type checking, and HIR lowering. It does not execute
the submitted program in the browser and does not include LLVM codegen.

The module is a standalone Emscripten build with no entry function. JavaScript provides the
`zith.host_write` import; the `wasi_snapshot_preview1.fd_write` stub forwards writes to that same
host callback so compiler and program streams can be rendered without a filesystem.

## Exports

| Export | Signature | Purpose |
|---|---|---|
| `zith_alloc` | `(size: i32) -> i32` | Allocate a byte buffer readable by the module. |
| `zith_free` | `(ptr: i32, size: i32) -> ()` | Free a buffer returned by `zith_alloc`. |
| `zith_compile_source` | `(ptr, len, mode, opt_level, emit_mask: i32) -> i32` | Check and emit compiler stages up to HIR. |
| `zith_run_source` | `(ptr: i32, len: i32) -> i32` | Alias of compile in run mode: check plus HIR output. Does not execute the program. |
| `zith_last_error_ptr` | `() -> i32` | Pointer to the accumulated error text from the last call. |
| `zith_last_error_len` | `() -> i32` | Byte length of the last error buffer. |
| `zith_last_output_ptr` | `() -> i32` | Pointer to compiler emission output from the last call. |
| `zith_last_output_len` | `() -> i32` | Byte length of the last output buffer. |
| `zith_error_count` | `() -> i32` | Number of rendered diagnostics from the last call. |
| `zith_error_at` | `(index: i32) -> i32` | Pointer to one rendered `severity: message` line, or `0`. |
| `zith_compiler_version_ptr` | `() -> i32` | Pointer to the compiler version string from `ZITH_VERSION`. |
| `zith_compiler_version_len` | `() -> i32` | Byte length of the compiler version string. |

## Imports

| Module | Import | Purpose |
|---|---|---|
| `zith` | `host_write(stream: i32, ptr: i32, len: i32)` | Render UTF-8 bytes to the playground terminal. Stream `1` is stdout, stream `2` is stderr. |
| `wasi_snapshot_preview1` | standard WASI imports | Stubs; `fd_write` forwards write buffers to `zith.host_write`. |

## Return Codes

| Code | Meaning |
|---|---|
| `0` | Success: source was checked and staged output was produced. |
| `1` | Compilation failure: diagnostics were rendered, and `last_error` is non-empty. |
| `2` | Invalid parameter: the call did not enter the compiler pipeline, and `last_error` is non-empty. |

Invalid parameters are reported before any session is created, so callers must check
`zith_last_error_ptr/len` instead of treating non-zero status as a compiler diagnostic.

## `mode`

The playground accepts only `0` for check and `1` for run. Broad compiler modes such as Debug,
Release, Fast, and Small are not part of the browser ABI; they are native CLI options.

## `opt_level`

`opt_level` must be in the range `0..3`. The playground does not perform native code generation but
keeps the range consistent with the CLI and C API.

## `emit_mask`

| Bit | Stage |
|---|---|
| `1` | Tokens |
| `2` | AST |
| `4` | HIR |
| `8` | IR |
| `16` | ASM |

IR and ASM require an LLVM backend, which is not available in this WASM build. Passing either bit
produces a diagnostic and return code `1`; it does not silently ignore the request.

## Diagnostics

`zith_error_at` returns one stable line per diagnostic, rendered as:

```text
severity: message
```

The line remains valid until the next `zith_compile_source` or `zith_run_source` call. An `index`
greater than or equal to `zith_error_count()` returns `0`. This stable line format is intended for
the playground; structured JSON diagnostics will be added by a future LSP-facing API.

## JavaScript Buffer Helpers

```js
function readString(instance, ptr, len) {
  return ptr && len ? new TextDecoder().decode(new Uint8Array(instance.exports.memory.buffer, ptr, len)) : "";
}

function compilerErrorAt(instance, index) {
  const ptr = instance.exports.zith_error_at(index);
  if (!ptr) return null;
  const length = instance.exports.memory.buffer.byteLength;
  const bytes = new Uint8Array(instance.exports.memory.buffer, ptr, length - ptr);
  const line = [];
  for (const byte of bytes) {
    if (byte === 0 || byte === 10) break;
    line.push(byte);
  }
  return new TextDecoder().decode(new Uint8Array(line));
}

function readCompilerOutput(instance) {
  return readString(
    instance,
    instance.exports.zith_last_output_ptr(),
    instance.exports.zith_last_output_len()
  );
}

function readCompilerError(instance) {
  const ptr = instance.exports.zith_last_error_ptr();
  return readString(instance, ptr, instance.exports.zith_last_error_len());
}

function compilerVersion(instance) {
  return readString(
    instance,
    instance.exports.zith_compiler_version_ptr(),
    instance.exports.zith_compiler_version_len()
  );
}
```
