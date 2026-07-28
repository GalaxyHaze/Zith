import sys

with open('src/session/compilation-session.cpp', 'r') as f:
    lines = f.readlines()

out = []
i = 0
while i < len(lines):
    line = lines[i]
    if 'exePath = exeDir + "/" + binName;' in line:
        out.append(line)
        out.append("    }\n")
        out.append("    \n")
        out.append("    auto &triple = mOpts.get().targetTriple;\n")
        out.append("    bool isWasm = triple.find(\"wasm32\") != std::string::npos || triple.find(\"wasm64\") != std::string::npos;\n")
        out.append("\n")
        out.append("    if (isWasm && !mOpts.get().outputFile.empty() && mOpts.get().outputFile != mObjectPath) {\n")
        out.append("        exePath = mOpts.get().outputFile;\n")
        out.append("    } else if (isWasm) {\n")
        out.append("        exePath += \".wasm\";\n")
        out.append("    } else {\n")
        i += 1
        continue
    if '#ifdef _WIN32' in line and lines[i+1].strip() == 'exePath += ".exe";':
        out.append(line)
        out.append(lines[i+1])
        out.append(lines[i+2])
        out.append("    }\n")
        i += 3
        continue
    
    if 'if (mOpts.get().flags.verbose())' in line and lines[i+1].strip() == 'writeOutput("  [link] %s -> %s\\n", mObjectPath.c_str(), exePath.c_str());':
        out.append(line)
        out.append(lines[i+1])
        out.append("\n")
        out.append("    std::string linkCmd;\n")
        out.append("    if (isWasm) {\n")
        out.append("        bool isWasi = triple.find(\"wasi\") != std::string::npos;\n")
        out.append("        linkCmd = \"wasm-ld \";\n")
        out.append("        if (!isWasi)\n")
        out.append("            linkCmd += \"--no-entry --export-all \";\n")
        out.append("        linkCmd += \"-o \" + exePath + \" \" + mObjectPath;\n")
        out.append("    } else {\n")
        out.append("        linkCmd = \"cc -o \" + exePath + \" \" + mObjectPath;\n")
        out.append("    }\n")
        i += 4 # skip the old linkCmd
        continue
        
    if 'if (mOpts.get().flags.verbose())' in line and lines[i+1].strip() == 'writeOutput("  [exec] %s\\n", exePath.c_str());':
        out.append(line)
        out.append(lines[i+1])
        out.append("\n")
        out.append("    if (isWasm) {\n")
        out.append("        if (mOpts.get().flags.verbose())\n")
        out.append("            writeOutput(\"  [exec] skipped (cannot natively execute WASM)\\n\");\n")
        out.append("        mChildExitCode = 0;\n")
        out.append("        return true;\n")
        out.append("    }\n")
        i += 2
        continue

    out.append(line)
    i += 1

with open('src/session/compilation-session.cpp', 'w') as f:
    f.writelines(out)
