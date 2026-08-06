#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zith::cinterop {

enum class TypeKind : uint8_t { Void, Bool, Integer, Float, Pointer, Record, Enum };

struct Type {
    TypeKind kind = TypeKind::Void;
    uint8_t bits  = 0;
    bool isSigned = false;
    bool isConst  = false;
    /// Plain C `char` (not `signed char`/`unsigned char`), lowered to Zith `char`.
    bool isChar = false;
    std::string name;
    std::shared_ptr<const Type> pointee;
};

struct Function {
    std::string name;
    std::string linkageName;
    std::vector<Type> parameters;
    Type result;
    bool isVariadic = false;
};

struct Diagnostic {
    std::string message;
    unsigned line   = 0;
    unsigned column = 0;
};

/// Immutable libclang result for a single C header and its transitive inputs.
struct CHeaderArtifact {
    std::string headerPath;
    std::vector<Function> functions;
    std::vector<std::string> dependencies;
    std::vector<Diagnostic> diagnostics;
    /// Declarations skipped because a parameter or result type is not representable.
    /// Kept out of `diagnostics` so one unsupported decl does not fail the import.
    std::vector<std::string> skippedFunctions;
};

struct ParseOptions {
    std::string targetTriple;
    std::string sysroot;
    std::vector<std::string> includeDirs;
    std::vector<std::string> defines;
};

[[nodiscard]] bool available() noexcept;
[[nodiscard]] std::shared_ptr<const CHeaderArtifact> parseHeader(const std::string &headerPath,
                                                                 const ParseOptions &options);

/// Directories the host C toolchain searches for system headers. Discovered once
/// per (target triple, sysroot) pair; empty when no directory can be determined.
[[nodiscard]] std::vector<std::string> systemIncludeDirs(const std::string &targetTriple,
                                                         const std::string &sysroot);

} // namespace zith::cinterop
