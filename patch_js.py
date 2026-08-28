import re

with open('js/playground.js', 'r') as f:
    js = f.read()

# Remove UI elements not in DOM anymore
js = re.sub(r'const runButton = .*?;', '', js)
js = re.sub(r'const releaseVersion = .*?;', '', js)
js = re.sub(r'const releaseDetail = .*?;', '', js)
js = re.sub(r'const releaseLink = .*?;', '', js)
js = re.sub(r'const runtimeStatus = .*?;', 'const runtimeStatusCompact = document.getElementById("runtime-status-compact");', js)
js = re.sub(r'const runtimeAsset = .*?;', '', js)
js = re.sub(r'const runtimeModule = .*?;', '', js)

# In loadRuntime
js = js.replace('releaseVersion.textContent = "Local build";', '')
js = js.replace('releaseDetail.textContent = "Loading ./zith-playground.wasm...";', '')
js = js.replace('releaseLink.href = LOCAL_WASM_URL;', '')
js = js.replace('releaseLink.textContent = "Open local module";', '')
js = js.replace('runtimeAsset.textContent = "zith-playground.wasm";', '')

js = js.replace('const requiredExports = ["memory", "zith_alloc", "zith_free", "zith_last_error_ptr", "zith_last_error_len", "zith_run_source"];', 'const requiredExports = ["memory", "zith_alloc", "zith_free", "zith_compile_source"];')
js = js.replace('runtimeModule.textContent = exports.join(", ");', '')

js = js.replace('runtimeStatus.textContent = "ABI incomplete";', 'runtimeStatusCompact.textContent = "Compiler: ABI Incomplete";')
js = js.replace('releaseDetail.textContent = `Missing exports: ${missingExports.join(", ")}.`;', '')

js = js.replace('runtimeStatus.textContent = "Native runtime missing";', 'runtimeStatusCompact.textContent = "Compiler: Native missing";')
js = js.replace('releaseDetail.textContent = `${nativeImports.length} C/C++ imports still need to be linked into the module.`;', '')

js = js.replace('runtimeStatus.textContent = "Ready";', 'runtimeStatusCompact.textContent = "Compiler: Ready (local)";')
js = js.replace('releaseDetail.textContent = "Local browser runtime is ready.";', '')
js = js.replace('runButton.disabled = false;', '')

js = js.replace('releaseVersion.textContent = "Unavailable";', '')
js = js.replace('releaseDetail.textContent = "The local WASM build could not be loaded.";', '')
js = js.replace('runtimeStatus.textContent = "Load failed";', 'runtimeStatusCompact.textContent = "Compiler: Load Failed";')


# Remove event listeners
js = re.sub(r'document\.getElementById\("clear-output"\)\.addEventListener\("click", \(\) => {.*?}\);', '', js, flags=re.DOTALL)
js = re.sub(r'document\.getElementById\("reset-code"\)\.addEventListener\("click", \(\) => {.*?}\);', '', js, flags=re.DOTALL)
js = re.sub(r'document\.getElementById\("copy-link"\)\.addEventListener\("click", async \(\) => {.*?}\);', '', js, flags=re.DOTALL)
js = re.sub(r'runButton\.addEventListener\("click", \(\) => {.*?}\);', '', js, flags=re.DOTALL)

terminal_js = '''
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

    const args = cmd.split(/\\s+/).filter(Boolean);
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
'''
# Replace the old terminal block, then append the new logic before the last few
# lines (which are savedCode and loadRuntime).
js = re.sub(r'const terminalInput = document\.getElementById\("terminal-input"\);\nconst commandHistory = \[\];\nlet historyIndex = -1;\n.*?\nconst savedCode = ', 'TERMINAL_MARKER\nconst savedCode = ', js, flags=re.DOTALL)
js = js.replace('TERMINAL_MARKER', terminal_js + '\n')
js = re.sub(r'\n{3,}', '\n\n', js)

with open('js/playground.js', 'w') as f:
    f.write(js)
