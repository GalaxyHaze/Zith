// WASM VM v2 runtime ABI smoke test.
//
// This harness uses the generated Emscripten glue so Asyncify-backed host
// input follows the same boundary as the browser playground. It exercises:
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
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const gluePath = process.argv[2];
const actualWasmPath = process.argv[3];
const stdlibPath = process.argv[4];
if (!gluePath || !actualWasmPath || !stdlibPath) {
  throw new Error(
    "usage: node tests/test-wasm-runtime.mjs <zith-playground.mjs> <zith-playground.wasm> <zith-stdlib.pack>",
  );
}

const stdoutChunks = [];
const stderrChunks = [];
let moduleRuntime;
let inputQueue = [];
let inputClosed = false;
let pendingInput = null;

function writeOut(text) {
  process.stdout.write(text);
}

function writeErr(text) {
  process.stderr.write(text);
}

function drainInput() {
  if (!pendingInput) return;
  if (inputQueue.length > 0) {
    const byte = inputQueue.shift();
    const wakeUp = pendingInput;
    pendingInput = null;
    queueMicrotask(() => wakeUp(byte));
  } else if (inputClosed) {
    const wakeUp = pendingInput;
    pendingInput = null;
    queueMicrotask(() => wakeUp(-1));
  }
}

function hostReadByte() {
  return moduleRuntime.Asyncify.handleSleep((wakeUp) => {
    if (pendingInput) throw new Error("WASM requested more than one input byte at a time.");
    pendingInput = wakeUp;
    drainInput();
  });
}

function queueInput(bytes) {
  inputQueue.push(...bytes);
  drainInput();
}

function closeInput() {
  inputClosed = true;
  drainInput();
}

function resetInput() {
  inputQueue = [];
  inputClosed = false;
  pendingInput = null;
}

async function loadInstance() {
  const wasmBytes = await readFile(actualWasmPath);
  const wasmModule = await WebAssembly.compile(wasmBytes);
  const { default: createModule } = await import(pathToFileURL(resolve(gluePath)).href);
  moduleRuntime = await createModule({
    wasmBinary: wasmBytes,
    instantiateWasm(imports, receiveInstance) {
      imports.zith ??= {};
      imports.zith.host_write = (stream, ptr, len) => {
        const view = new Uint8Array(moduleRuntime.HEAPU8.buffer, ptr, len);
        const text = Buffer.from(view).toString("utf8");
        if (stream === 1) {
          stdoutChunks.push(text);
          writeOut(text);
        } else {
          stderrChunks.push(text);
          writeErr(text);
        }
      };
      imports.zith.host_read_byte = hostReadByte;
      const wasmInstance = new WebAssembly.Instance(wasmModule, imports);
      return receiveInstance(wasmInstance);
    },
  });

  const exports = new Proxy(
    {},
    {
      get(_target, property) {
        if (property === "memory") {
          return {
            get buffer() {
              return moduleRuntime.HEAPU8.buffer;
            },
          };
        }
        if (typeof property === "string") return moduleRuntime[`_${property}`];
        return undefined;
      },
    },
  );
  return { exports };
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

async function runSourceAsync(source) {
  const { ptr, len } = writeString(source);
  try {
    return await moduleRuntime.ccall(
      "zith_run_source",
      "number",
      ["number", "number"],
      [ptr, len],
      { async: true },
    );
  } finally {
    instance.exports.zith_free(ptr, len);
  }
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

const dynDispatchSource = `interface Biased {
    fn value(self, bias: i32): i32
}
struct First {
    base: i32,
    fn value(self, bias: i32): i32 { self.base + bias }
}
struct Second {
    base: i32,
    fn value(self, bias: i32): i32 { self.base * bias }
}
fn apply(value: dyn Biased, bias: i32): i32 {
    return value.value(bias);
}
fn main(): i32 {
    return apply(First { base: 10 }, 3) + apply(Second { base: 20 }, 3);
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

  stdoutChunks.length = 0;
  resetInput();
  const inputSource = `from std/io/console
from soon/memory

fn main(): i32 {
    let heap = HeapAllocator {};
    _ = print(heap, "ready");
    var longLine = input(heap);
    if (not longLine.good()) { return 1; }
    let longLength = longLine.len();
    let longText = longLine.text();
    var index: u64 = 0;
    for (index < longLength) {
        if (raw longText[index] != ('x' as char)) { return 4; }
        index = index + 1;
    }
    longLine.destroy();
    var partialLine = input(heap);
    if (not partialLine.good()) { return 2; }
    let partialLength = partialLine.len();
    let partialText = partialLine.text();
    if (raw partialText[0u64] != ('a' as char) or
        raw partialText[1u64] != ('b' as char) or
        raw partialText[2u64] != ('c' as char)) { return 5; }
    partialLine.destroy();
    var eofLine = input(heap);
    if (eofLine.good()) { return 3; }
    eofLine.destroy();
    (longLength + partialLength) as i32
}
`;
  const inputRun = runSourceAsync(inputSource);
  if (!pendingInput) throw new Error("guest did not suspend while waiting for input");
  assertEqual(stdoutChunks.join(""), "ready", "guest output is flushed before input wait");
  queueInput(Buffer.from(`${"x".repeat(1024)}\nabc`, "utf8"));
  await new Promise((resolveTurn) => setTimeout(resolveTurn, 0));
  if (!pendingInput) throw new Error("partial final line did not wait for EOF");
  closeInput();
  assertEqual(await inputRun, 0, "interactive input program completes");
  assertEqual(Number(instance.exports.zith_exit_code()), 1027,
              "long line and partial final line are returned before EOF");
  assertEqual(stdoutChunks.join(""), "ready", "guest output is flushed before input and EOF");

  resetInput();
  const cancelledRun = runSourceAsync(`from std/io/console
from soon/memory

fn main(): i32 {
    let heap = HeapAllocator {};
    var line = input(heap);
    if (line.good()) { return 1; }
    0
}
`);
  if (!pendingInput) throw new Error("guest did not suspend before cancellation");
  const cancelInput = pendingInput;
  pendingInput = null;
  cancelInput(-2);
  assertEqual(await cancelledRun, 6, "host cancellation has its own ABI status");

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
  assertEqual(Number(instance.exports.zith_exit_code()), 7, "extern fn main exit code");

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
  assertEqual(Number(instance.exports.zith_exit_code()), 10, "add4 computes 10 as main exit code");

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
  assertEqual(Number(instance.exports.zith_exit_code()), 13, "bitwise operations exit code (1+8+4=13)");

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
  assertEqual(Number(instance.exports.zith_exit_code()), 20,
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
  assertEqual(Number(instance.exports.zith_exit_code()), 5050,
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
  assertEqual(Number(instance.exports.zith_exit_code()), 0,
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
  assertEqual(Number(instance.exports.zith_exit_code()), 0,
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
  assertEqual(Number(instance.exports.zith_exit_code()), 9, "run_source exit code");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const dynHir = compileSource(dynDispatchSource);
  assertEqual(runHir(dynHir), 0, "dyn dispatch executes from flat HIR");
  assertEqual(Number(instance.exports.zith_exit_code()), 73,
              "flat HIR dyn dispatch selects both implementations");

  stdoutChunks.length = 0;
  stderrChunks.length = 0;
  const dynSource = writeString(dynDispatchSource);
  assertEqual(instance.exports.zith_run_source(dynSource.ptr, dynSource.len), 0,
              "dyn dispatch executes through run_source");
  assertEqual(Number(instance.exports.zith_exit_code()), 73,
              "run_source dyn dispatch selects both implementations");

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
    assertEqual(Number(instance.exports.zith_exit_code()), 5, "cached replay exit code");
  }
  assertEqual(Number(instance.exports.zith_hir_cache_hits() - hitsBefore), 2,
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
