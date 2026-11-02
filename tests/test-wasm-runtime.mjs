// WASM VM v2 runtime ABI smoke test.
//
// This harness is intentionally free of Emscripten glue. It loads the
// compiler/runtime .wasm, supplies only the zith.host_write import, and
// exercises the public execution ABI:
//
//   zith_emit_hir(ptr, len)     -> HIR flat blob, then
//   zith_execute_hir(ptr, len)  -> lowers HIR flat to VM v2 IR and runs it.
//   zith_compile_hir(ptr, len)  -> compiles once and stores the flat blob in
//                                  the artifact cache, then
//   zith_execute_cached(ptr, len) -> replays the cached blob without recompiling.
//   zith_register_stdlib_pack(ptr, len) -> registers host-delivered stdlib sources.
//
// The valid fixtures mirror the host VM v2 acceptance slice: stdlib println
// and manual extern fn puts.

import { readFile } from "node:fs/promises";
import { Buffer } from "node:buffer";

const wasmPath = process.argv[2];
const stdlibPath = process.argv[3];
if (!wasmPath || !stdlibPath) {
  throw new Error("usage: node tests/test-wasm-runtime.mjs <zith-playground.wasm> <zith-stdlib.pack>");
}

const stdoutChunks = [];
const stderrChunks = [];

function writeOut(text) {
  process.stdout.write(text);
}

function writeErr(text) {
  process.stderr.write(text);
}

async function loadInstance() {
  const bytes = await readFile(wasmPath);
  const module = await WebAssembly.compile(bytes);
  const imports = WebAssembly.Module.imports(module);
  const wasiNames = new Set(
    imports
      .filter((entry) => entry.module === "wasi_snapshot_preview1" && entry.kind === "function")
      .map((entry) => entry.name),
  );
  const wasi = Object.fromEntries([...wasiNames].map((name) => [name, () => 0]));
  const importObject = {
    zith: {
      host_write(stream, ptr, len) {
        const view = new Uint8Array(instance.exports.memory.buffer, ptr, len);
        const text = Buffer.from(view).toString("utf8");
        if (stream === 1) {
          stdoutChunks.push(text);
          writeOut(text);
        } else {
          stderrChunks.push(text);
          writeErr(text);
        }
      },
    },
    wasi_snapshot_preview1: wasi,
    env: Object.fromEntries(
      imports
        .filter((entry) => entry.module === "env" && entry.kind === "function")
        .map((entry) => [entry.name, () => 0]),
    ),
  };
  return WebAssembly.instantiate(module, importObject);
}

// Keep instance scoped to the import callback via a mutable binding.
let instance;

function readCString(ptr) {
  const view = new Uint8Array(instance.exports.memory.buffer);
  let end = ptr;
  while (end < view.length && view[end] !== 0) end += 1;
  return Buffer.from(view.subarray(ptr, end)).toString("utf8");
}

function writeString(text) {
  const bytes = Buffer.from(text, "utf8");
  const ptr = instance.exports.zith_alloc(bytes.length);
  new Uint8Array(instance.exports.memory.buffer, ptr, bytes.length).set(bytes);
  return { ptr, len: bytes.length };
}

function writeBytes(bytes) {
  const ptr = instance.exports.zith_alloc(bytes.length);
  new Uint8Array(instance.exports.memory.buffer, ptr, bytes.length).set(bytes);
  return { ptr, len: bytes.length };
}

function readBuffer(ptr, len) {
  const copy = new Uint8Array(instance.exports.memory.buffer, ptr, len);
  return Buffer.from(copy);
}

function readLastOutput() {
  return readBuffer(
    instance.exports.zith_last_output_ptr(),
    instance.exports.zith_last_output_len(),
  ).toString("utf8");
}

function assertEqual(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${String(expected)}, got ${String(actual)}`);
  }
}

function compileSource(source) {
  const { ptr, len } = writeString(source);
  const status = instance.exports.zith_emit_hir(ptr, len);
  if (status !== 0) {
    const errorLen = instance.exports.zith_last_error_len();
    const errorPtr = instance.exports.zith_last_error_ptr();
    throw new Error(`zith_emit_hir failed: ${readCString(errorPtr)}`);
  }
  const hirPtr = instance.exports.zith_last_buffer_ptr();
  const hirLen = instance.exports.zith_last_buffer_len();
  return readBuffer(hirPtr, hirLen);
}

function runHir(hir) {
  const ptr = instance.exports.zith_alloc(hir.length);
  new Uint8Array(instance.exports.memory.buffer, ptr, hir.length).set(hir);
  return instance.exports.zith_execute_hir(ptr, hir.length);
}

const stdHello = `from std/io/console
from std/memory

fn main(): i32 {
    let heap = HeapAllocator {}
    _ = println(heap, "Hello, WASM!");
    0
}
`;

const externHello = `extern fn puts(msg: *char)

fn main(): i32 {
    _ = puts("hello-puts");
    7
}
`;

// VM v2 lowers pointer dereference, so the previous `*(&x)` fixture is no
// longer unsupported. An external global still has no VM v2 representation and
// keeps the unsupported-construct status path covered.
const unsupportedSource = `extern var external_value: i32;

fn main() {
    _ = external_value;
}
`;

async function main() {
  instance = await loadInstance();
  const stdlib = await readFile(stdlibPath);
  const stdlibBuffer = writeBytes(stdlib);
  assertEqual(instance.exports.zith_register_stdlib_pack(stdlibBuffer.ptr, stdlibBuffer.len), 0,
              "stdlib pack registration");

  const dumpSource = writeString(`fn main(): i32 {
    1 + 2
}
`);
  const dumpStatus = instance.exports.zith_compile_source(
    dumpSource.ptr,
    dumpSource.len,
    0,
    0,
    4 | 32 | 64,
  );
  assertEqual(dumpStatus, 0, "HIR, CST and VIR compile emission");
  const dumpOutput = readLastOutput();
  if (!dumpOutput.includes("--- CST ---")) {
    throw new Error("CST header missing from zith_last_output buffer");
  }
  if (!dumpOutput.includes("--- HIR ---")) {
    throw new Error("HIR header missing from zith_last_output buffer");
  }
  if (!dumpOutput.includes("--- VIR ---") || !dumpOutput.includes("fn main")) {
    throw new Error("VIR content missing from zith_last_output buffer");
  }
  stdoutChunks.length = 0;
  stderrChunks.length = 0;

  const invalidMaskSource = writeString("fn main() {}\n");
  const invalidMaskStatus = instance.exports.zith_compile_source(
    invalidMaskSource.ptr,
    invalidMaskSource.len,
    0,
    0,
    128,
  );
  assertEqual(invalidMaskStatus, 2, "unknown emit_mask bits are rejected");
  const errorLen = instance.exports.zith_last_error_len();
  const errorPtr = instance.exports.zith_last_error_ptr();
  const invalidMaskError = readBuffer(errorPtr, errorLen).toString("utf8");
  if (!invalidMaskError.includes("invalid emit_mask")) {
    throw new Error("unknown emit_mask bit did not report a parameter error");
  }
  stdoutChunks.length = 0;
  stderrChunks.length = 0;

  const externHir = compileSource(externHello);
  const externStatus = runHir(externHir);
  assertEqual(externStatus, 0, "extern fn puts program runs");
  assertEqual(stdoutChunks.join(""), "hello-puts\n", "puts output");
  assertEqual(instance.exports.zith_exit_code(), 7n, "extern fn main exit code");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const hir = compileSource(stdHello);
  const status = runHir(hir);
  assertEqual(status, 0, "stdlib println program runs");
  assertEqual(stdoutChunks.join(""), "Hello, WASM!\n", "println output");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const printSource = writeString(`from std/io/console
from std/memory

fn add4(a: i32, b: i32, c: i32, d: i32): i32 {
    a + b + c + d
}

fn main(): i32 {
    let heap = HeapAllocator {}
    print(heap, "one-");
    print(heap, "two");
    add4(1, 2, 3, 4)
}
`);
  assertEqual(instance.exports.zith_run_source(printSource.ptr, printSource.len), 0,
              "print and 4-arg function execute through VM v2");
  assertEqual(stdoutChunks.join(""), "one-two", "print outputs continuous text without newline");
  assertEqual(instance.exports.zith_exit_code(), 10n, "add4 computes 10 as main exit code");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const unsupportedHir = compileSource(unsupportedSource);
  const unsupportedStatus = runHir(unsupportedHir);
  assertEqual(unsupportedStatus, 5, "unsupported construct returns 5");
  assertEqual(stderrChunks.join(""),
              "unsupported external global 'external_value' in v2 VM\n",
              "unsupported message is reported");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const bitwiseSource = writeString(`fn main(): i32 {
    let a = 5 &. 3;
    let b = 1 << 3;
    let c = 16 >> 2;
    a + b + c
}
`);
  assertEqual(instance.exports.zith_run_source(bitwiseSource.ptr, bitwiseSource.len), 0,
              "bitwise operations execute through WASM VM v2");
  assertEqual(instance.exports.zith_exit_code(), 13n, "bitwise operations exit code (1+8+4=13)");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const controlArrayText = `fn main(): i32 {
    let values: [3]i32 = [10, 20, 30];
    let index: i32 = 1;
    if (index == 1) {
        return raw values[index];
    }
    0
}
`;
  const controlArrayHir = compileSource(controlArrayText);
  assertEqual(runHir(controlArrayHir), 0,
              "control flow and dynamic array index execute through WASM VM v2");
  assertEqual(instance.exports.zith_exit_code(), 20n,
              "dynamic array index returns the selected element in WASM");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const controlFlowSource = `fn main(): i32 {
    var x = 0;
    for (i in 0..100) {
        x += i;
    }
    x
}
`;
  const controlFlowBuffer = writeString(controlFlowSource);
  assertEqual(instance.exports.zith_run_source(controlFlowBuffer.ptr, controlFlowBuffer.len), 0,
              "for loop executes through the playground WASM ABI");
  assertEqual(instance.exports.zith_exit_code(), 5050n,
              "for loop sums the range 0..100");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const variadicSource = writeString(`extern fn malloc(size: u64): raw opaque
extern fn snprintf(buf: *char, size: u64, fmt: *char, ...): i32
extern fn puts(msg: *char): i32

fn main(): i32 {
    var buf: *char = malloc(64) as *char;
    _ = snprintf(buf, 64, "%u %u %u %u %u", 1, 2, 3, 4, 5);
    _ = puts(buf);
    0
}
`);
  assertEqual(instance.exports.zith_run_source(variadicSource.ptr, variadicSource.len), 0,
              "variadic snprintf executes through the playground WASM ABI");
  assertEqual(stdoutChunks.join(""), "1 2 3 4 5\n",
              "WASM variadic snprintf preserves the complete argument range");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const printfSource = writeString(`extern fn printf(msg: *char, ...)

fn main() {
    printf("Hello World!");
}
`);
  assertEqual(instance.exports.zith_run_source(printfSource.ptr, printfSource.len), 0,
              "variadic printf executes through the playground WASM ABI");
  assertEqual(stdoutChunks.join(""), "Hello World!",
              "WASM variadic printf writes the complete message");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const reallocSource = writeString(`extern fn malloc(size: u64): *char
extern fn realloc(ptr: *char, size: u64): *char
extern fn free(ptr: *char)

fn main(): i32 {
    var ptr: *char = malloc(8);
    ptr = realloc(ptr, 16);
    free(ptr);
    0
}
`);
  assertEqual(instance.exports.zith_run_source(reallocSource.ptr, reallocSource.len), 0,
              "realloc executes through the playground WASM ABI");
  assertEqual(instance.exports.zith_exit_code(), 0n,
              "realloc program returns successfully in WASM");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const callocSource = writeString(`extern fn calloc(count: u64, size: u64): *char
extern fn strncmp(left: *char, right: *char, count: u64): i32

fn main(): i32 {
    var block: *char = calloc(4, 8);
    if (strncmp(block, block, 8) != 0) {
        return 1;
    }
    0
}
`);
  assertEqual(instance.exports.zith_run_source(callocSource.ptr, callocSource.len), 0,
              "calloc and strncmp execute through the playground WASM ABI");
  assertEqual(instance.exports.zith_exit_code(), 0n,
              "calloc-backed block compares equal to itself in WASM");

  const outOfBoundsText = `fn main(): i32 {
    let values: [2]i32 = [10, 20];
    let index: i32 = 2;
    raw values[index]
}
`;
  const outOfBoundsHir = compileSource(outOfBoundsText);
  assertEqual(runHir(outOfBoundsHir), 3,
              "dynamic array index traps out of bounds in WASM");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const runSource = writeString(`extern fn puts(msg: *char)

fn main(): i32 {
    _ = puts("run-source");
    9
}
`);
  assertEqual(instance.exports.zith_run_source(runSource.ptr, runSource.len), 0,
              "run_source executes through VM v2");
  assertEqual(stdoutChunks.join(""), "run-source\n", "run_source output");
  assertEqual(instance.exports.zith_exit_code(), 9n, "run_source exit code");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const cacheSource = writeString(`extern fn puts(msg: *char)

fn main(): i32 {
    _ = puts("cache-once");
    5
}
`);
  const hitsBefore = instance.exports.zith_hir_cache_hits();
  assertEqual(instance.exports.zith_compile_hir(cacheSource.ptr, cacheSource.len), 0,
              "compile_hir stores an artifact");
  for (let replay = 0; replay < 2; replay++) {
    stdoutChunks.length = 0;
    assertEqual(instance.exports.zith_execute_cached(cacheSource.ptr, cacheSource.len), 0,
                "cached replay succeeds");
    assertEqual(stdoutChunks.join(""), "cache-once\n", "cached replay output");
    assertEqual(instance.exports.zith_exit_code(), 5n, "cached replay exit code");
  }
  assertEqual(instance.exports.zith_hir_cache_hits() - hitsBefore, 2n,
              "both replays hit the artifact cache");

  const missSource = writeString("fn main(): i32 {\n    1\n}\n");
  assertEqual(instance.exports.zith_execute_cached(missSource.ptr, missSource.len), 1,
              "cache miss reports a compile failure status");
  const missJsonLen = instance.exports.zith_last_diagnostics_json_len();
  const missJsonPtr = instance.exports.zith_last_diagnostics_json_ptr();
  const missJson = readBuffer(missJsonPtr, missJsonLen).toString("utf8");
  if (!missJson.includes("HIR artifact cache miss")) {
    throw new Error("cache miss was not reported through the diagnostics channel");
  }
}

await main();
writeOut("wasm-runtime: ok\n");
