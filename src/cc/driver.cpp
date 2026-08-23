#include "driver.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <unordered_set>

#ifdef ZITH_HAS_LLVM
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#endif

#ifndef ZITH_ENABLE_C_COMPILE
#define ZITH_ENABLE_C_COMPILE 0
#endif

#ifndef ZITH_C_COMPILE_AVAILABLE
#define ZITH_C_COMPILE_AVAILABLE 0
#endif

#ifndef ZITH_LLD_AVAILABLE
#define ZITH_LLD_AVAILABLE 0
#endif

#if ZITH_C_COMPILE_AVAILABLE
#include <libtcc.h>
#endif

#if ZITH_LLD_AVAILABLE
#include <lld/Common/Driver.h>
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(macho)
#endif

#ifdef _WIN32
#include <process.h>
#elif !defined(ZITH_IS_WASM)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace zith::cc {

namespace {

const char *compilerDriverPath() {
#ifdef ZITH_C_COMPILER_PATH
    return ZITH_C_COMPILER_PATH;
#elif defined(_WIN32)
    return "clang";
#else
    return "/usr/bin/cc";
#endif
}

int captureProgram(const std::vector<std::string> &arguments, std::string &output) {
#if defined(_WIN32) || defined(ZITH_IS_WASM)
    (void)arguments;
    (void)output;
    return -1;
#else
    if (arguments.empty())
        return -1;

    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0)
        return -1;

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv.front(), argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t bytes = read(pipefd[0], buffer.data(), buffer.size());
        if (bytes == 0)
            break;
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        output.append(buffer.data(), static_cast<size_t>(bytes));
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
#endif
}

int normalizeExitStatus(const int status) {
#ifdef _WIN32
    return status;
#else
    if (status == -1)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
#endif
}

[[maybe_unused]] bool targetLooksLikeWasm(const std::string &triple) {
    return triple.find("wasm32") != std::string::npos || triple.find("wasm64") != std::string::npos;
}

[[maybe_unused]] bool isHostCompatibleTriple(const std::string &targetTriple) {
    if (targetTriple.empty())
        return true;
#ifdef ZITH_HAS_LLVM
    llvm::Triple target(targetTriple);
    llvm::Triple host(llvm::sys::getDefaultTargetTriple());
    return target.getArch() == host.getArch() && target.getOS() == host.getOS() &&
           target.getObjectFormat() == host.getObjectFormat();
#else
    return true;
#endif
}

#if ZITH_C_COMPILE_AVAILABLE
void appendTccError(void *opaque, const char *message) {
    auto *output = static_cast<std::string *>(opaque);
    if (message != nullptr) {
        *output += message;
        output->push_back('\n');
    }
}

CCompileResult compileWithTcc(const CCompileRequest &request) {
    CCompileResult result;
    result.backend = CCompileBackend::LibTcc;

    if (!isHostCompatibleTriple(request.targetTriple)) {
        result.diagnostics.push_back({DiagnosticKind::TccUnsupportedTriple,
                                      "libtcc is available, but target triple '" +
                                          request.targetTriple +
                                          "' is not supported by the embedded backend; disable "
                                          "internal C compilation to use "
                                          "the external compiler path instead.",
                                      request.inputPath});
        return result;
    }

    TCCState *state = tcc_new();
    if (state == nullptr) {
        result.diagnostics.push_back(
            {DiagnosticKind::TccUnavailable, "failed to initialize libtcc", request.inputPath});
        return result;
    }

    std::string tccOutput;
    tcc_set_error_func(state, &tccOutput, appendTccError);
    tcc_set_output_type(state, TCC_OUTPUT_OBJ);

    if (!request.sysroot.empty())
        tcc_set_options(state, ("--sysroot=" + request.sysroot).c_str());
    for (const auto &directory : request.includeDirs)
        tcc_add_include_path(state, directory.c_str());
    for (const auto &define : request.defines) {
        const auto equal = define.find('=');
        if (equal == std::string::npos)
            tcc_define_symbol(state, define.c_str(), "1");
        else
            tcc_define_symbol(state, define.substr(0, equal).c_str(),
                              define.substr(equal + 1).c_str());
    }

    result.commandDisplay = "libtcc -c " + request.inputPath + " -o " + request.outputPath;
    const int addFile     = tcc_add_file(state, request.inputPath.c_str());
    const int emitObj     = addFile == 0 ? tcc_output_file(state, request.outputPath.c_str()) : -1;
    if (addFile != 0 || emitObj != 0) {
        result.toolOutput = std::move(tccOutput);
        result.diagnostics.push_back({DiagnosticKind::CCompilationFailed,
                                      "C compilation failed for '" + request.inputPath + "'",
                                      request.inputPath});
        tcc_delete(state);
        return result;
    }

    tcc_delete(state);
    result.ok = true;
    return result;
}

CCompileResult compileWithEmbeddedBackend(const CCompileRequest &request) {
    return compileWithTcc(request);
}
#else
[[maybe_unused]] CCompileResult compileWithEmbeddedBackend(const CCompileRequest &request) {
    (void)request;
    return {};
}
#endif

[[maybe_unused]] CCompileResult compileWithExternalCc(const CCompileRequest &request) {
    CCompileResult result;
    result.backend = CCompileBackend::ExternalCc;

    std::vector<std::string> arguments = {compilerDriverPath()};
    if (!request.targetTriple.empty())
        arguments.push_back("--target=" + request.targetTriple);
    if (!request.sysroot.empty())
        arguments.push_back("--sysroot=" + request.sysroot);
    arguments.push_back("-c");
    arguments.push_back(request.inputPath);
    arguments.push_back("-o");
    arguments.push_back(request.outputPath);
    for (const auto &directory : request.includeDirs)
        arguments.push_back("-I" + directory);
    for (const auto &define : request.defines)
        arguments.push_back("-D" + define);

    result.commandDisplay = displayCommand(arguments);
    const int status      = captureProgram(arguments, result.toolOutput);
    if (normalizeExitStatus(status) != 0) {
        result.diagnostics.push_back({DiagnosticKind::CCompilationFailed,
                                      "C compilation failed for '" + request.inputPath + "'",
                                      request.inputPath});
        return result;
    }

    result.ok = true;
    return result;
}

LinkResult linkWithExternalCc(const LinkRequest &request) {
    LinkResult result;
    result.backend = LinkBackend::ExternalCc;

    std::vector<std::string> arguments = {compilerDriverPath()};
    if (!request.targetTriple.empty())
        arguments.push_back("--target=" + request.targetTriple);
    if (!request.sysroot.empty())
        arguments.push_back("--sysroot=" + request.sysroot);
    arguments.push_back("-o");
    arguments.push_back(request.outputPath);
    arguments.push_back(request.primaryObjectPath);
    arguments.insert(arguments.end(), request.extraObjectPaths.begin(),
                     request.extraObjectPaths.end());
    for (const auto &directory : request.libraryDirs)
        arguments.push_back("-L" + directory);
    for (const auto &library : request.libraries) {
        if (!isValidLibraryName(library)) {
            result.diagnostics.push_back(
                {DiagnosticKind::InvalidLibraryName, "invalid library name '" + library + "'", {}});
            return result;
        }
        arguments.push_back("-l" + library);
    }

    result.commandDisplay = displayCommand(arguments);
    const int status      = captureProgram(arguments, result.toolOutput);
    if (normalizeExitStatus(status) != 0) {
        result.diagnostics.push_back({DiagnosticKind::LinkFailed,
                                      "linking failed for '" + request.outputPath + "'",
                                      request.outputPath});
        return result;
    }

    result.ok = true;
    return result;
}

#if ZITH_LLD_AVAILABLE
bool targetUsesElf(const std::string &triple) {
#ifdef ZITH_HAS_LLVM
    llvm::Triple parsed(triple.empty() ? llvm::sys::getDefaultTargetTriple() : triple);
    return parsed.isOSBinFormatELF();
#else
    return true;
#endif
}

LinkResult linkWithLld(const LinkRequest &request) {
    LinkResult result;
    result.backend = LinkBackend::LldApi;

    if (!targetUsesElf(request.targetTriple)) {
        result.diagnostics.push_back({DiagnosticKind::LldUnavailable,
                                      "embedded LLD is only wired for ELF targets in this build",
                                      request.outputPath});
        return result;
    }

    std::vector<std::string> arguments = {"ld.lld", "-o", request.outputPath,
                                          request.primaryObjectPath};
    arguments.insert(arguments.end(), request.extraObjectPaths.begin(),
                     request.extraObjectPaths.end());
    if (!request.sysroot.empty())
        arguments.push_back("--sysroot=" + request.sysroot);
    for (const auto &directory : request.libraryDirs)
        arguments.push_back("-L" + directory);
    for (const auto &library : request.libraries) {
        if (!isValidLibraryName(library)) {
            result.diagnostics.push_back(
                {DiagnosticKind::InvalidLibraryName, "invalid library name '" + library + "'", {}});
            return result;
        }
        arguments.push_back("-l" + library);
    }

    result.commandDisplay = displayCommand(arguments);

    std::vector<const char *> argv;
    argv.reserve(arguments.size());
    for (const auto &argument : arguments)
        argv.push_back(argument.c_str());

    std::string stdoutText;
    std::string stderrText;
    llvm::raw_string_ostream stdoutStream(stdoutText);
    llvm::raw_string_ostream stderrStream(stderrText);
    const lld::DriverDef drivers[] = {{lld::Gnu, &lld::elf::link}};
    const lld::Result lldResult =
        lld::lldMain(llvm::ArrayRef<const char *>(argv), stdoutStream, stderrStream, drivers);
    stdoutStream.flush();
    stderrStream.flush();
    result.toolOutput = stdoutText + stderrText;
    if (lldResult.retCode != 0) {
        result.diagnostics.push_back({DiagnosticKind::LinkFailed,
                                      "embedded LLD failed for '" + request.outputPath + "'",
                                      request.outputPath});
        return result;
    }

    result.ok = true;
    return result;
}
#endif

} // namespace

bool isValidLibraryName(const std::string_view name) {
    if (name.empty())
        return false;
    return std::all_of(name.begin(), name.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '+' ||
               character == '-' || character == '.';
    });
}

std::string displayCommand(const std::vector<std::string> &arguments) {
    std::string command;
    for (const auto &argument : arguments) {
        if (!command.empty())
            command += ' ';
        command += argument;
    }
    return command;
}

DiscoveryResult discoverCSources(const std::vector<std::string> &roots) {
    DiscoveryResult result;
    namespace fs = std::filesystem;

    std::unordered_set<std::string> seen;
    for (const auto &root : roots) {
        std::error_code error;
        if (!fs::exists(root, error) || !fs::is_directory(root, error)) {
            result.ok = false;
            result.diagnostics.push_back(
                {DiagnosticKind::MissingSourceRoot, "C source root does not exist: " + root, root});
            continue;
        }

        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            error);
        fs::recursive_directory_iterator end;
        if (error) {
            result.ok = false;
            result.diagnostics.push_back(
                {DiagnosticKind::SourceDiscoveryFailed,
                 "failed to scan C source root: " + root + " (" + error.message() + ")", root});
            continue;
        }

        while (it != end) {
            const auto current = *it;
            if (current.is_regular_file(error) && current.path().extension() == ".c") {
                const std::string canonical = fs::weakly_canonical(current.path()).string();
                if (seen.insert(canonical).second)
                    result.sourcePaths.push_back(canonical);
            }

            it.increment(error);
            if (error) {
                result.ok = false;
                result.diagnostics.push_back(
                    {DiagnosticKind::SourceDiscoveryFailed,
                     "failed while scanning C source root: " + root + " (" + error.message() + ")",
                     root});
                error.clear();
            }
        }
    }

    std::sort(result.sourcePaths.begin(), result.sourcePaths.end());
    return result;
}

CCompileResult compileCSource(const CCompileRequest &request) {
    namespace fs = std::filesystem;

    if (!fs::exists(request.inputPath)) {
        return {false,
                CCompileBackend::None,
                {},
                {},
                {{DiagnosticKind::CCompilationFailed,
                  "C source does not exist: " + request.inputPath, request.inputPath}}};
    }

#if !ZITH_ENABLE_C_COMPILE
    return {false,
            CCompileBackend::None,
            {},
            {},
            {{DiagnosticKind::CCompileDisabled,
              "this build was configured with ZITH_ENABLE_C_COMPILE=OFF", request.inputPath}}};
#else
    if (ZITH_C_COMPILE_AVAILABLE) {
        CCompileResult result = compileWithEmbeddedBackend(request);
        if (result.ok || !result.diagnostics.empty())
            return result;
    }
    return compileWithExternalCc(request);
#endif
}

LinkResult linkNative(const LinkRequest &request) {
#if ZITH_LLD_AVAILABLE
    if (!targetLooksLikeWasm(request.targetTriple))
        return linkWithLld(request);
#endif
    return linkWithExternalCc(request);
}

} // namespace zith::cc
