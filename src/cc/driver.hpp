#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zith::cc {

enum class CCompileBackend : uint8_t { None, ExternalCc, LibTcc };
enum class LinkBackend : uint8_t { None, LldApi, ExternalCc, WasmLd };

enum class DiagnosticKind : uint8_t {
    TccUnavailable,
    TccUnsupportedTriple,
    LldUnavailable,
    CCompileDisabled,
    CCompilationFailed,
    LinkFailed,
    MissingSourceRoot,
    SourceDiscoveryFailed,
    InvalidLibraryName,
};

struct Diagnostic {
    DiagnosticKind kind = DiagnosticKind::SourceDiscoveryFailed;
    std::string message;
    std::string path;
};

struct DiscoveryResult {
    bool ok = true;
    std::vector<std::string> sourcePaths;
    std::vector<Diagnostic> diagnostics;
};

struct CCompileRequest {
    std::string inputPath;
    std::string outputPath;
    std::string targetTriple;
    std::string sysroot;
    std::vector<std::string> includeDirs;
    std::vector<std::string> defines;
    bool verbose = false;
};

struct CCompileResult {
    bool ok                 = false;
    CCompileBackend backend = CCompileBackend::None;
    std::string commandDisplay;
    std::string toolOutput;
    std::vector<Diagnostic> diagnostics;
};

struct LinkRequest {
    std::string outputPath;
    std::string targetTriple;
    std::string sysroot;
    std::string primaryObjectPath;
    std::vector<std::string> extraObjectPaths;
    std::vector<std::string> libraryDirs;
    std::vector<std::string> libraries;
    bool verbose = false;
};

struct LinkResult {
    bool ok             = false;
    LinkBackend backend = LinkBackend::None;
    std::string commandDisplay;
    std::string toolOutput;
    std::vector<Diagnostic> diagnostics;
};

DiscoveryResult discoverCSources(const std::vector<std::string> &roots);
CCompileResult compileCSource(const CCompileRequest &request);
LinkResult linkNative(const LinkRequest &request);
bool isValidLibraryName(std::string_view name);
std::string displayCommand(const std::vector<std::string> &arguments);

} // namespace zith::cc
