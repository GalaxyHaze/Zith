#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zith::cinterop {

enum class TypeKind : uint8_t { Void, Bool, Integer, Float, Pointer, Record, Enum };

struct Type;

struct RecordField {
    std::string name;
    std::shared_ptr<Type> type;
    uint64_t offsetBits = 0;
};

struct Type {
    TypeKind kind = TypeKind::Void;
    uint8_t bits  = 0;
    bool isSigned = false;
    bool isConst  = false;
    /// Plain C `char` (not `signed char`/`unsigned char`), lowered to Zith `char`.
    bool isChar = false;
    std::string name;
    /// Clang's target-resolved C spelling, used when emitting C ABI adapters.
    std::string cSpelling;
    std::shared_ptr<const Type> pointee;
    /// For an enum, the underlying integer type reported by libclang. Null when
    /// the enum has no complete definition, in which case a by-value use cannot
    /// be proven and is rejected by the lowerer.
    std::shared_ptr<const Type> enumUnderlying;
    /// Validated record ABI.  Only populated when libclang proved the layout for
    /// the target named in `ParseOptions::targetTriple`.
    std::vector<RecordField> recordFields;
    uint64_t sizeBytes     = 0;
    uint64_t alignBytes    = 0;
    bool hasVerifiedLayout = false;
    /// Legacy direct LLVM coercion metadata. Records called through generated
    /// C ABI adapters do not depend on this classification.
    bool abiIsSingleI64 = false;
    /// Nested-only shape used while validating a larger record. A one-field
    /// `int` record has a layout but cannot yet be passed to C by value unless
    /// the outer record classifies it as one of two 32-bit slots.
    bool abiIsSingleI32 = false;
};

struct Function {
    std::string name;
    std::string linkageName;
    std::string adapterName;
    std::string adapterThunkName;
    std::string adapterSource;
    std::vector<Type> parameters;
    Type result;
    bool isVariadic = false;
};

struct Global {
    std::string name;
    std::string linkageName;
    Type type;
    /// C object constness, distinct from const qualification within pointer types.
    bool isConst = false;
};

struct Typedef {
    std::string name;
    Type type;
};

enum class ConstantKind : uint8_t { Integer, Float, Bool, Char };

/// One object-like C macro that expands to an exact scalar literal importable
/// by Zith.  The value is stored ready to lower; no external evaluation is used.
struct Constant {
    std::string name;
    ConstantKind kind         = ConstantKind::Integer;
    uint8_t bits              = 32;
    bool isSigned             = true;
    std::int64_t integerValue = 0;
    double floatValue         = 0.0;
    bool boolValue            = false;
    char charValue            = '\0';
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
    std::vector<Global> globals;
    std::vector<Typedef> typedefs;
    std::vector<std::string> dependencies;
    std::vector<Diagnostic> diagnostics;
    /// Non-fatal diagnostics for declarations that could not be imported.
    std::vector<Diagnostic> skippedDeclarations;
    /// Object-like scalar macros imported as module constants. They are visible
    /// to the module that imports this header, like the external functions.
    std::vector<Constant> constants;
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
