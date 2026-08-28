const WASM_URL = new URL("../playground/zith-playground.wasm", location.href);
const RUNTIME_MANIFEST_URL = new URL("../playground/runtime.json", location.href);
const DEFAULT_SOURCE = `fn main() {
}`;

const editor = document.getElementById("source-code");
const lineNumbers = document.getElementById("line-numbers");
const cursorPosition = document.getElementById("cursor-position");
const output = document.getElementById("output");




const runtimeStatusCompact = document.getElementById("runtime-status-compact");


let runtime = null;
const encoder = new TextEncoder();
const decoder = new TextDecoder();

function escapeHtml(value) {
    return value.replace(/[&<>"]/g, character => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[character]);
}

function writeOutput(message, className = "") {
    const prefix = output.textContent.trim() ? "\n" : "";
    output.insertAdjacentHTML("beforeend", `${prefix}<span class="${className}">${escapeHtml(message)}</span>`);
    document.getElementById("terminal-container").scrollTop = document.getElementById("terminal-container").scrollHeight;
    output.scrollTop = output.scrollHeight;
}

function updateEditorMeta() {
    const lines = editor.value.split("\n");
    lineNumbers.textContent = lines.map((_, index) => index + 1).join("\n");
    const beforeCursor = editor.value.slice(0, editor.selectionStart);
    const line = beforeCursor.split("\n").length;
    const column = beforeCursor.length - beforeCursor.lastIndexOf("\n");
    cursorPosition.textContent = `Ln ${line}, Col ${column}`;
}

async function readVersionManifest() {
    try {
        const response = await fetch(RUNTIME_MANIFEST_URL);
        if (!response.ok) return null;
        const manifest = await response.json();
        return typeof manifest.version === "string" ? manifest.version : null;
    } catch (_) {
        return null;
    }
}

function buildImportObject(module, instanceRef, writeOutput) {
    const imports = WebAssembly.Module.imports(module);
    const usedModules = new Set(imports.map(entry => entry.module));
    const importObject = {};

    const hostWrite = (stream, pointer, length) => {
        if (!instanceRef.instance || length < 0) return;
        const bytes = new Uint8Array(instanceRef.instance.exports.memory.buffer, pointer, length);
        writeOutput(decoder.decode(bytes), stream === 2 ? "terminal-error" : "terminal-ok");
    };

    const syscallStubs = {
        __syscall_getcwd: () => -1,
        __syscall_readlinkat: () => -1,
        __syscall_unlinkat: () => -1,
        __syscall_rmdir: () => -1
    };

    const wasiStubs = {
        clock_time_get: (clockId, precision, timePointer) => {
            if (!instanceRef.instance) return 8;
            const memory = instanceRef.instance.exports.memory;
            const now = BigInt(Date.now()) * 1000000n;
            new BigUint64Array(memory.buffer, timePointer, 1)[0] = now;
            return 0;
        },
        fd_write: (fd, iovecsPointer, iovecsLength, writtenPointer) => {
            if (!instanceRef.instance) return 8;
            const memory = instanceRef.instance.exports.memory;
            const iov = new DataView(memory.buffer, iovecsPointer, iovecsLength * 8);
            let total = 0;
            for (let index = 0; index < iovecsLength; index += 1) {
                const pointer = iov.getUint32(index * 8, true);
                const length = iov.getUint32(index * 8 + 4, true);
                if (length > 0) {
                    const bytes = new Uint8Array(memory.buffer, pointer, length);
                    writeOutput(decoder.decode(bytes), fd === 2 ? "terminal-error" : "terminal-ok");
                    total += length;
                }
            }
            if (writtenPointer && total <= 0xffffffff) {
                new Uint32Array(memory.buffer, writtenPointer, 1)[0] = total;
            }
            return 0;
        },
        fd_read: () => 8,
        fd_fdstat_get: () => 8,
        fd_prestat_get: () => 8,
        fd_prestat_dir_name: () => 8,
        fd_readdir: () => 8,
        fd_close: () => 0,
        fd_seek: () => 0,
        args_get: () => 8,
        args_sizes_get: () => 8,
        environ_get: () => 8,
        environ_sizes_get: () => 8,
        path_create_directory: () => 8,
        path_filestat_get: () => 8,
        path_open: () => 8,
        path_readlink: () => 8,
        random_get: () => 8,
        proc_exit: () => { throw new Error("WebAssembly compiler exited."); }
    };

    if (usedModules.has("zith")) importObject.zith = { host_write: hostWrite };
    if (usedModules.has("wasi_snapshot_preview1")) importObject.wasi_snapshot_preview1 = wasiStubs;
    if (usedModules.has("env")) importObject.env = syscallStubs;
    return importObject;
}

async function loadRuntime() {
    try {
        runtimeStatusCompact.textContent = "Compiler: Loading…";
        writeOutput("Fetching local WebAssembly build…", "terminal-dim");

        const wasmResponse = await fetch(WASM_URL);
        if (!wasmResponse.ok) throw new Error(`Local module returned ${wasmResponse.status}.`);
        const module = await WebAssembly.compile(await wasmResponse.arrayBuffer());
        const exports = WebAssembly.Module.exports(module).map(entry => entry.name);
        const requiredExports = ["memory", "zith_alloc", "zith_free", "zith_compile_source"];
        const missingExports = requiredExports.filter(name => !exports.includes(name));

        if (missingExports.length) {
            runtimeStatusCompact.textContent = "Compiler: ABI Incomplete";
            writeOutput(`The local build is missing: ${missingExports.join(", ")}.`, "terminal-error");
            return;
        }

        const instanceRef = {};
        const importObject = buildImportObject(module, instanceRef, writeOutput);
        const instance = await WebAssembly.instantiate(module, importObject);
        instanceRef.instance = instance;

        const version = await readVersionManifest();
        const displayVersion = version ? `release ${version}` : "local";
        runtime = instance.exports;
        runtimeStatusCompact.textContent = `Compiler: Ready (${displayVersion})`;
        writeOutput(`WebAssembly compiler ready (${displayVersion}).`, "terminal-ok");
    } catch (error) {
        const message = error instanceof Error ? error.message : "Unknown loading error.";
        runtimeStatusCompact.textContent = "Compiler: Load Failed";
        writeOutput(`Could not load the WebAssembly compiler: ${message}`, "terminal-error");
    }
}

editor.addEventListener("input", updateEditorMeta);
editor.addEventListener("click", updateEditorMeta);
editor.addEventListener("keyup", updateEditorMeta);
editor.addEventListener("scroll", () => { lineNumbers.scrollTop = editor.scrollTop; });

const terminalInput = document.getElementById("terminal-input");
const commandHistory = [];
let historyIndex = -1;

terminalInput.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
        const cmd = terminalInput.value.trim();
        terminalInput.value = "";
        if (cmd) {
            commandHistory.push(cmd);
            historyIndex = commandHistory.length;
            executeCommand(cmd);
        }
    } else if (e.key === "ArrowUp") {
        if (historyIndex > 0) {
            historyIndex--;
            terminalInput.value = commandHistory[historyIndex];
        }
        e.preventDefault();
    } else if (e.key === "ArrowDown") {
        if (historyIndex < commandHistory.length - 1) {
            historyIndex++;
            terminalInput.value = commandHistory[historyIndex];
        } else {
            historyIndex = commandHistory.length;
            terminalInput.value = "";
        }
        e.preventDefault();
    }
});

function executeCommand(cmd) {
    writeOutput(`> ${cmd}`, "terminal-dim");

    if (cmd === "clear") {
        output.textContent = "";
        return;
    }

    if (cmd === "help") {
        writeOutput(`Available commands:
  zithc check [--opt-level <0|1|2|3>] [--emit hir]
  zithc run [--opt-level <0|1|2|3>] [--emit hir]
  zithc build [--opt-level <0|1|2|3>]
  clear
  help`, "terminal-ok");
        return;
    }

    const args = cmd.split(/\s+/).filter(Boolean);
    const first = args[0];
    const subcommand = first === "zithc" ? args[1] : first;
    const commandArgs = first === "zithc" ? args.slice(2) : args.slice(1);

    if (subcommand === "run" || subcommand === "check" || subcommand === "build") {
        if (!runtime) {
            writeOutput("Compiler not loaded.", "terminal-error");
            return;
        }
        if (!runtime.zith_compile_source) {
            writeOutput("Compiler ABI incompatible (missing zith_compile_source).", "terminal-error");
            return;
        }

        let parsed = null;
        try {
            parsed = parseCompilerArgs(subcommand, commandArgs);
        } catch (error) {
            writeOutput(error.message, "terminal-error");
            return;
        }
        const mode = subcommand === "run" ? 1 : 0;
        runCompiler(mode, parsed.optLevel, parsed.emitMask, subcommand);
        return;
    }

    writeOutput(`zithc: command not found: ${first || subcommand}`, "terminal-error");
}

function parseCompilerArgs(subcommand, args) {
    let optLevel = 0;
    let emitMask = 0;

    for (let i = 0; i < args.length; i++) {
        if (args[i] === "--opt-level" && i + 1 < args.length) {
            const raw = args[i + 1];
            const value = Number.parseInt(raw, 10);
            if (!Number.isInteger(value) || value < 0 || value > 3) {
                throw new Error(`zithc: invalid optimization level: ${raw}`);
            }
            optLevel = value;
            i++;
        } else if (args[i] === "--emit" && i + 1 < args.length) {
            const emits = args[i + 1].split(",");
            for (const emit of emits) {
                if (emit !== "hir") {
                    throw new Error(`zithc: emitter '${emit}' is unavailable in this browser WASM build; only --emit hir is supported.`);
                }
                emitMask = 4;
            }
            i++;
        } else {
            throw new Error(`zithc: unexpected argument for ${subcommand}: ${args[i]}`);
        }
    }

    return { optLevel, emitMask };
}

function runCompiler(mode, optLevel, emitMask, subcommand) {
    let pointer = 0;
    try {
        const sourceText = editor.value;
        const source = encoder.encode(sourceText);
        pointer = runtime.zith_alloc(source.length);
        if (!pointer) throw new Error("The WASM allocator returned a null pointer.");
        new Uint8Array(runtime.memory.buffer, pointer, source.length).set(source);

        const result = runtime.zith_compile_source(pointer, source.length, mode, optLevel, emitMask);
        const lastError = readLastError();

        if (result === 0) {
            if (subcommand === "check") {
                writeOutput("check passed", "terminal-ok");
            } else if (subcommand === "build") {
                writeOutput("build complete (browser WASM simulator stops after HIR)", "terminal-ok");
            } else {
                writeOutput("compiled successfully (browser WASM does not execute program output)", "terminal-ok");
            }
        } else {
            if (lastError) writeOutput(lastError, "terminal-error");
            if (subcommand === "check") {
                writeOutput(`check failed (Status: ${result})`, "terminal-error");
            } else if (subcommand === "build") {
                writeOutput(`build failed (Status: ${result})`, "terminal-error");
            } else {
                writeOutput(`compile error (Status: ${result})`, "terminal-error");
            }
        }
    } catch (error) {
        const message = error instanceof Error ? error.message : "Unknown runtime error.";
        writeOutput(`Execution failed: ${message}`, "terminal-error");
    } finally {
        if (pointer) runtime.zith_free(pointer, encoder.encode(editor.value).length);
    }
}

function readLastError() {
    if (!runtime || !runtime.zith_last_error_ptr || !runtime.zith_last_error_len) return "";
    const pointer = runtime.zith_last_error_ptr();
    const length = runtime.zith_last_error_len();
    if (!pointer || !length) return "";
    return decoder.decode(new Uint8Array(runtime.memory.buffer, pointer, length));
}

const savedCode = new URLSearchParams(window.location.hash.slice(1)).get("code");
if (savedCode) { try { editor.value = decodeURIComponent(escape(atob(savedCode))); } catch (_) { /* Ignore malformed share links. */ } }
updateEditorMeta();
loadRuntime();

document.getElementById("terminal-container").addEventListener("click", () => {
    document.getElementById("terminal-input").focus();
});
