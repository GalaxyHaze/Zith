import sys

with open('src/session/compilation-session.cpp', 'r') as f:
    text = f.read()

start = text.find('bool CompilationSession::linkAndExec() {')
end = text.find('std::string CompilationSession::fmtStage() {')

if start != -1 and end != -1:
    new_func = """bool CompilationSession::linkAndExec() {
#ifndef ZITH_IS_WASM
    if (mObjectPath.empty())
        return false;

    // Determine executable path
    std::string exePath;
    if (!mOpts.get().outputFile.empty() && mOpts.get().outputFile != mObjectPath) {
        exePath = mOpts.get().outputFile;
    } else {
        namespace fs = std::filesystem;
        // Determine binary name
        std::string binName;
        if (!mProjectConfig.name.empty())
            binName = mProjectConfig.name;
        else
            binName = fs::path(mFilePath).stem().string();

        // Compute output directory
        std::string exeDir = (fs::path(mProjectRoot) / "target").string();
        // Cross-compilation: target/<triple>/ subdirectory
        auto &triple = mOpts.get().targetTriple;
        if (!triple.empty()) {
#ifdef ZITH_HAS_LLVM
            std::string hostTriple = llvm::sys::getDefaultTargetTriple();
#else
            std::string hostTriple;
#endif
            if (triple != hostTriple)
                exeDir = (fs::path(mProjectRoot) / "target" / triple).string();
        }

        fs::create_directories(exeDir);
        exePath = exeDir + "/" + binName;
    }

    auto &triple = mOpts.get().targetTriple;
    bool isWasm = triple.find("wasm32") != std::string::npos || triple.find("wasm64") != std::string::npos;

    if (isWasm && !mOpts.get().outputFile.empty() && mOpts.get().outputFile != mObjectPath) {
        exePath = mOpts.get().outputFile;
    } else if (isWasm) {
        exePath += ".wasm";
    } else {
#ifdef _WIN32
        if (mOpts.get().outputFile.empty() || mOpts.get().outputFile == mObjectPath)
            exePath += ".exe";
#endif
    }

    if (mOpts.get().flags.verbose())
        writeOutput("  [link] %s -> %s\\n", mObjectPath.c_str(), exePath.c_str());

    std::string linkCmd;
    if (isWasm) {
        bool isWasi = triple.find("wasi") != std::string::npos;
        linkCmd = "wasm-ld ";
        if (!isWasi)
            linkCmd += "--no-entry --export-all ";
        linkCmd += "-o " + exePath + " " + mObjectPath;
    } else {
        linkCmd = "/usr/bin/cc -o " + exePath + " " + mObjectPath;
    }
    
    if (!mOpts.get().sysroot.empty())
        linkCmd += " --sysroot=" + mOpts.get().sysroot;
    
    if (mOpts.get().flags.verbose())
        writeOutput("  [link] %s\\n", linkCmd.c_str());

    int linkResult = std::system(linkCmd.c_str());
    if (linkResult != 0) {
        writeOutput("%s[error]%s linking failed (exit code %d)\\n", ansicolor("\\033[31m"),
                    ansicolor("\\033[0m"), linkResult);
        return false;
    }

    if (mOpts.get().flags.verbose())
        writeOutput("  [exec] %s\\n", exePath.c_str());

    if (isWasm) {
        if (mOpts.get().flags.verbose())
            writeOutput("  [exec] skipped (cannot natively execute WASM)\\n");
        mChildExitCode = 0;
        return true;
    }

    // Execute the binary and propagate its exit code
    int execResult = std::system(exePath.c_str());
    if (execResult == -1) {
        writeOutput("%s[error]%s failed to launch executable\\n", ansicolor("\\033[31m"),
                    ansicolor("\\033[0m"));
        return false;
    }

#ifdef _WIN32
    mChildExitCode = execResult;
#else
    if (WIFEXITED(execResult)) {
        mChildExitCode = WEXITSTATUS(execResult);
    } else if (WIFSIGNALED(execResult)) {
        mChildExitCode = 128 + WTERMSIG(execResult);
    } else {
        mChildExitCode = 1;
    }
#endif

    return true;
#else
    (void)mObjectPath;
    writeOutput("%s[error]%s cannot execute on WASM target\\n", ansicolor("\\033[31m"),
                ansicolor("\\033[0m"));
    return false;
#endif
}

"""
    text = text[:start] + new_func + text[end:]
    with open('src/session/compilation-session.cpp', 'w') as f:
        f.write(text)
