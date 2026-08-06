#include "cinterop/c-header.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>

#ifdef ZITH_ENABLE_C_INTEROP
#include <clang-c/Index.h>
#endif

namespace zith::cinterop {
namespace {

#ifdef ZITH_ENABLE_C_INTEROP

std::string takeString(const CXString value) {
    const char *text   = clang_getCString(value);
    std::string result = text != nullptr ? text : "";
    clang_disposeString(value);
    return result;
}

bool isMainFile(const CXCursor cursor, const CXFile main_file) {
    CXFile file = nullptr;
    clang_getSpellingLocation(clang_getCursorLocation(cursor), &file, nullptr, nullptr, nullptr);
    return file != nullptr && clang_File_isEqual(file, main_file) != 0;
}

bool lowerType(const CXType source, Type &result, std::string &unsupported) {
    const CXType type = clang_getCanonicalType(source);
    result.isConst    = clang_isConstQualifiedType(source) != 0;
    switch (type.kind) {
    case CXType_Void:
        result.kind = TypeKind::Void;
        return true;
    case CXType_Bool:
        result.kind = TypeKind::Bool;
        result.bits = 1;
        return true;
    case CXType_Char_S:
        result.kind   = TypeKind::Integer;
        result.isChar = true;
        result.bits   = static_cast<uint8_t>(clang_Type_getSizeOf(type) * 8);
        return result.bits != 0;
    case CXType_SChar:
    case CXType_Short:
    case CXType_Int:
    case CXType_Long:
    case CXType_LongLong:
    case CXType_Int128:
        result.kind     = TypeKind::Integer;
        result.isSigned = true;
        result.bits     = static_cast<uint8_t>(clang_Type_getSizeOf(type) * 8);
        return result.bits != 0;
    case CXType_Char_U:
        result.kind   = TypeKind::Integer;
        result.isChar = true;
        result.bits   = static_cast<uint8_t>(clang_Type_getSizeOf(type) * 8);
        return result.bits != 0;
    case CXType_UChar:
    case CXType_UShort:
    case CXType_UInt:
    case CXType_ULong:
    case CXType_ULongLong:
    case CXType_UInt128:
        result.kind     = TypeKind::Integer;
        result.isSigned = false;
        result.bits     = static_cast<uint8_t>(clang_Type_getSizeOf(type) * 8);
        return result.bits != 0;
    case CXType_Float:
    case CXType_Double:
    case CXType_LongDouble:
    case CXType_Float16:
    case CXType_Float128:
        result.kind = TypeKind::Float;
        result.bits = static_cast<uint8_t>(clang_Type_getSizeOf(type) * 8);
        return result.bits != 0;
    case CXType_Pointer: {
        const CXType pointee_type      = clang_getPointeeType(type);
        const CXType pointee_canonical = clang_getCanonicalType(pointee_type);
        // Function pointers would need a Zith callable/ABI model to be passed as
        // values. Import them as an opaque pointer so declarations stay usable.
        if (pointee_canonical.kind == CXType_FunctionProto ||
            pointee_canonical.kind == CXType_FunctionNoProto) {
            result.kind    = TypeKind::Pointer;
            result.pointee = std::make_shared<Type>(Type{});
            return true;
        }
        Type pointee;
        if (!lowerType(pointee_type, pointee, unsupported))
            return false;
        result.kind    = TypeKind::Pointer;
        result.pointee = std::make_shared<Type>(std::move(pointee));
        return true;
    }
    case CXType_Record:
        result.kind = TypeKind::Record;
        result.name = takeString(clang_getTypeSpelling(source));
        return true;
    case CXType_Enum:
        result.kind = TypeKind::Enum;
        result.name = takeString(clang_getTypeSpelling(source));
        return true;
    default:
        unsupported = takeString(clang_getTypeSpelling(source));
        return false;
    }
}

struct ParseState {
    CXFile mainFile = nullptr;
    CHeaderArtifact &artifact;
};

void collectInclusion(const CXFile included_file, CXSourceLocation *, const unsigned,
                      CXClientData data) {
    auto &artifact  = *static_cast<CHeaderArtifact *>(data);
    const auto path = takeString(clang_getFileName(included_file));
    if (path.empty())
        return;

    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    artifact.dependencies.push_back(error ? path : canonical.generic_string());
}

CXChildVisitResult visitCursor(const CXCursor cursor, const CXCursor, CXClientData data) {
    auto &state = *static_cast<ParseState *>(data);
    if (clang_getCursorKind(cursor) != CXCursor_FunctionDecl ||
        !isMainFile(cursor, state.mainFile)) {
        return CXChildVisit_Continue;
    }

    const auto linkage = clang_getCursorLinkage(cursor);
    if (linkage != CXLinkage_External && linkage != CXLinkage_UniqueExternal)
        return CXChildVisit_Continue;
    if (clang_Cursor_getStorageClass(cursor) == CX_SC_Static ||
        clang_Cursor_isFunctionInlined(cursor) != 0) {
        return CXChildVisit_Continue;
    }

    Function function;
    function.name        = takeString(clang_getCursorSpelling(cursor));
    function.linkageName = takeString(clang_Cursor_getMangling(cursor));
    if (function.linkageName.empty())
        function.linkageName = function.name;

    // C has no overloading, but glibc repeats several stdio functions under
    // `__isoc99_*` AsmLabelAttr symbols so the *first* declaration is authoritative.
    for (const auto &existing : state.artifact.functions) {
        if (existing.name == function.name) {
            state.artifact.skippedFunctions.push_back(function.name + ": duplicate declaration");
            return CXChildVisit_Continue;
        }
    }

    const auto type     = clang_getCursorType(cursor);
    function.isVariadic = clang_isFunctionTypeVariadic(type) != 0;

    std::string unsupported;
    if (!lowerType(clang_getResultType(type), function.result, unsupported)) {
        state.artifact.skippedFunctions.push_back(function.name + ": unsupported return type '" +
                                                  unsupported + "'");
        return CXChildVisit_Continue;
    }

    const int arguments = clang_Cursor_getNumArguments(cursor);
    for (unsigned index = 0; index < static_cast<unsigned>(arguments); ++index) {
        Type parameter;
        // The function type carries the declared parameter types after array/function
        // decay (e.g. `char[20]` -> `char *`, `__gnuc_va_list` -> `struct __va_list_tag *`),
        // which is exactly the ABI the binding needs.
        const CXType parameter_type = clang_getArgType(type, index);
        if (!lowerType(parameter_type, parameter, unsupported)) {
            // Some libclang builds expose the pre-decay array type here even though
            // the function type already carries the pointer ABI. Accept arrays as
            // opaque pointers so `char buf[20]`-style declarations stay callable.
            const CXType parameter_canonical = clang_getCanonicalType(parameter_type);
            if (parameter_canonical.kind == CXType_ConstantArray ||
                parameter_canonical.kind == CXType_IncompleteArray ||
                parameter_canonical.kind == CXType_VariableArray) {
                parameter         = Type{};
                parameter.kind    = TypeKind::Pointer;
                parameter.pointee = std::make_shared<Type>(Type{});
                function.parameters.push_back(std::move(parameter));
                continue;
            }
            state.artifact.skippedFunctions.push_back(
                function.name + ": unsupported parameter type '" + unsupported + "'");
            return CXChildVisit_Continue;
        }
        function.parameters.push_back(std::move(parameter));
    }
    state.artifact.functions.push_back(std::move(function));
    return CXChildVisit_Continue;
}

#endif

#ifdef ZITH_ENABLE_C_INTEROP

/// Records the parent directory of every directly included probe header.
void collectProbeDirectory(const CXFile included_file, CXSourceLocation *, const unsigned depth,
                           CXClientData data) {
    if (depth != 1U)
        return;
    auto &directories = *static_cast<std::vector<std::string> *>(data);
    const auto path   = takeString(clang_getFileName(included_file));
    if (path.empty())
        return;

    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    auto directory       = (error ? std::filesystem::path(path) : canonical).parent_path();
    if (directory.empty())
        return;
    auto text = directory.generic_string();
    if (std::find(directories.begin(), directories.end(), text) == directories.end())
        directories.push_back(std::move(text));
}

std::vector<std::string> probeSystemIncludeDirs(const std::string &targetTriple,
                                                const std::string &sysroot) {
    static constexpr const char *kProbeSource = "#include <stddef.h>\n"
                                                "#include <stdarg.h>\n"
                                                "#include <stdint.h>\n"
                                                "#include <stdio.h>\n"
                                                "#include <stdlib.h>\n"
                                                "#include <string.h>\n"
                                                "#include <limits.h>\n";

    std::vector<std::string> arguments{"-x", "c", "-std=c17"};
#ifdef ZITH_CLANG_RESOURCE_INCLUDE_DIR
    arguments.push_back("-I" ZITH_CLANG_RESOURCE_INCLUDE_DIR);
#endif
    if (!targetTriple.empty())
        arguments.push_back("--target=" + targetTriple);
    if (!sysroot.empty())
        arguments.push_back("--sysroot=" + sysroot);

    std::vector<const char *> argv;
    argv.reserve(arguments.size());
    for (const auto &argument : arguments)
        argv.push_back(argument.c_str());

    CXUnsavedFile probe{};
    probe.Filename = "zith-sysprobe.c";
    probe.Contents = kProbeSource;
    probe.Length   = static_cast<unsigned long>(std::char_traits<char>::length(kProbeSource));

    const CXIndex index    = clang_createIndex(0, 0);
    CXTranslationUnit unit = nullptr;
    const auto error       = clang_parseTranslationUnit2(
        index, probe.Filename, argv.data(), static_cast<int>(argv.size()), &probe, 1,
        CXTranslationUnit_DetailedPreprocessingRecord, &unit);

    std::vector<std::string> directories;
    // Parse diagnostics are ignored on purpose: a partial probe still reports the
    // directories it did manage to resolve.
    if (error == CXError_Success && unit != nullptr) {
        clang_getInclusions(unit, collectProbeDirectory, &directories);
        clang_disposeTranslationUnit(unit);
    }
    clang_disposeIndex(index);
    return directories;
}

#endif

std::vector<std::string> fallbackSystemIncludeDirs(const std::string &sysroot) {
    std::vector<std::string> directories;
    for (const char *candidate : {"/usr/include", "/usr/local/include"}) {
        std::filesystem::path path =
            sysroot.empty()
                ? std::filesystem::path(candidate)
                : std::filesystem::path(sysroot) / std::filesystem::path(candidate).relative_path();
        std::error_code error;
        if (std::filesystem::is_directory(path, error))
            directories.push_back(path.generic_string());
    }
    return directories;
}

} // namespace

bool available() noexcept {
#ifdef ZITH_ENABLE_C_INTEROP
    return true;
#else
    return false;
#endif
}

std::shared_ptr<const CHeaderArtifact> parseHeader(const std::string &headerPath,
                                                   const ParseOptions &options) {
    auto artifact        = std::make_shared<CHeaderArtifact>();
    artifact->headerPath = headerPath;
    artifact->dependencies.push_back(headerPath);
#ifndef ZITH_ENABLE_C_INTEROP
    (void)options;
    artifact->diagnostics.push_back(
        {"automatic C header imports require a native build with libclang; use 'extern fn' "
         "instead",
         0, 0});
    return artifact;
#else
    std::vector<std::string> arguments{"-x", "c", "-std=c17"};
#ifdef ZITH_CLANG_RESOURCE_INCLUDE_DIR
    arguments.push_back("-I" ZITH_CLANG_RESOURCE_INCLUDE_DIR);
#endif
    if (!options.targetTriple.empty())
        arguments.push_back("--target=" + options.targetTriple);
    if (!options.sysroot.empty())
        arguments.push_back("--sysroot=" + options.sysroot);
    for (const auto &directory : options.includeDirs)
        arguments.push_back("-I" + directory);
    for (const auto &define : options.defines)
        arguments.push_back("-D" + define);

    std::vector<const char *> argv;
    argv.reserve(arguments.size());
    for (const auto &argument : arguments)
        argv.push_back(argument.c_str());

    const CXIndex index    = clang_createIndex(0, 0);
    CXTranslationUnit unit = nullptr;
    const auto error       = clang_parseTranslationUnit2(
        index, headerPath.c_str(), argv.data(), static_cast<int>(argv.size()), nullptr, 0,
        CXTranslationUnit_DetailedPreprocessingRecord, &unit);
    if (error != CXError_Success || unit == nullptr) {
        artifact->diagnostics.push_back(
            {"failed to parse C header with libclang (error " + std::to_string(error) + ")", 0, 0});
        clang_disposeIndex(index);
        return artifact;
    }

    const auto diagnostic_count = clang_getNumDiagnostics(unit);
    for (unsigned index_value = 0; index_value < diagnostic_count; ++index_value) {
        const CXDiagnostic diagnostic = clang_getDiagnostic(unit, index_value);
        const auto severity           = clang_getDiagnosticSeverity(diagnostic);
        if (severity >= CXDiagnostic_Error) {
            CXFile file     = nullptr;
            unsigned line   = 0;
            unsigned column = 0;
            clang_getSpellingLocation(clang_getDiagnosticLocation(diagnostic), &file, &line,
                                      &column, nullptr);
            artifact->diagnostics.push_back(
                {takeString(
                     clang_formatDiagnostic(diagnostic, clang_defaultDiagnosticDisplayOptions())),
                 line, column});
        }
        clang_disposeDiagnostic(diagnostic);
    }

    const auto main_file = clang_getFile(unit, headerPath.c_str());
    ParseState state{main_file, *artifact};
    clang_visitChildren(clang_getTranslationUnitCursor(unit), visitCursor, &state);
    clang_getInclusions(unit, collectInclusion, artifact.get());
    std::sort(artifact->dependencies.begin(), artifact->dependencies.end());
    artifact->dependencies.erase(
        std::unique(artifact->dependencies.begin(), artifact->dependencies.end()),
        artifact->dependencies.end());

    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
    return artifact;
#endif
}

std::vector<std::string> systemIncludeDirs(const std::string &targetTriple,
                                           const std::string &sysroot) {
#ifdef ZITH_IS_WASM
    (void)targetTriple;
    (void)sysroot;
    return {};
#else
    const std::string key = targetTriple + '\0' + sysroot;

    static std::mutex cache_mutex;
    // Intentionally leaked: the cache lives for the whole process and must not be
    // torn down at exit while other translation units may still query it.
    static auto &cache = *new std::map<std::string, std::vector<std::string>>();

    const std::lock_guard<std::mutex> lock(cache_mutex);
    if (const auto cached = cache.find(key); cached != cache.end())
        return cached->second;

    std::vector<std::string> directories;
#ifdef ZITH_ENABLE_C_INTEROP
    directories = probeSystemIncludeDirs(targetTriple, sysroot);
#endif
    if (directories.empty())
        directories = fallbackSystemIncludeDirs(sysroot);
#ifdef ZITH_CLANG_RESOURCE_INCLUDE_DIR
    if (std::find(directories.begin(), directories.end(), ZITH_CLANG_RESOURCE_INCLUDE_DIR) ==
        directories.end()) {
        directories.push_back(ZITH_CLANG_RESOURCE_INCLUDE_DIR);
    }
#endif
    return cache.emplace(key, std::move(directories)).first->second;
#endif
}

} // namespace zith::cinterop
