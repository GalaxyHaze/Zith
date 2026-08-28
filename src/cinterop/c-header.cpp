#include "cinterop/c-header.hpp"

#include "support/int-literal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
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

bool isCompilerPredefined(std::string_view name) {
    // Clang's predefined macros are visited as MacroDefinition cursors with
    // (for user `__LINE__` redefinitions) the same source file.  Filtering the
    // classic double-underscore names keeps those out of Zith's module scope.
    if (name == "__LINE__" || name == "__FILE__" || name == "__DATE__"
        || name == "__TIME__" || name == "__TIMESTAMP__" || name == "__COUNTER__"
        || name == "__BASE_FILE__" || name == "__INCLUDE_LEVEL__")
        return true;
    if (name.size() >= 4U && name.starts_with("__") && name.ends_with("__"))
        return true;
    return false;
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
    CXTranslationUnit unit = nullptr;
    CHeaderArtifact &artifact;
};

struct Scalars {
    ConstantKind kind  = ConstantKind::Integer;
    bool isSigned      = true;
    uint8_t bits       = 32;
    std::int64_t intValue = 0;
    double floatValue  = 0.0;
    bool boolValue     = false;
    char charValue     = '\0';
};

[[nodiscard]] bool parseCharacterLiteral(std::string_view token, char &out) {
    if (token.size() < 3U || token.front() != '\'' || token.back() != '\'')
        return false;
    const std::string_view body = token.substr(1U, token.size() - 2U);
    if (body.size() != 1U)
        return false;
    out = body.front();
    return true;
}

[[nodiscard]] bool scalarFromTokens(std::string_view token, Scalars &out) {
    if (token == "true") {
        out.kind      = ConstantKind::Bool;
        out.boolValue = true;
        return true;
    }
    if (token == "false") {
        out.kind      = ConstantKind::Bool;
        out.boolValue = false;
        return true;
    }
    if (token.size() >= 2U && token.front() == '\'') {
        if (!parseCharacterLiteral(token, out.charValue))
            return false;
        out.kind = ConstantKind::Char;
        return true;
    }

    // Keep the suffix list and fit checks in sync with the Zith literal types.
    constexpr std::string_view kIntSuffixes[] = {
        "usize", "u64", "u32", "u16", "u8",
        "isize", "i64", "i32", "i16", "i8",
    };
    for (const auto suffix : kIntSuffixes) {
        if (!token.ends_with(suffix))
            continue;
        std::int64_t parsed             = 0;
        const std::string_view digits = token.substr(0U, token.size() - suffix.size());
        const support::IntLiteralStatus status = support::parseIntegerLiteral(digits, parsed);
        if (status == support::IntLiteralStatus::Overflow)
            return false;
        if (status == support::IntLiteralStatus::NotInteger)
            return false;
        const bool is_native = suffix == "isize" || suffix == "usize";
        std::uint64_t bits   = 64;
        if (!is_native) {
            const std::string_view width = suffix.substr(1U);
            if (width == "8")
                bits = 8;
            else if (width == "16")
                bits = 16;
            else if (width == "32")
                bits = 32;
        }
        const bool signed_type = !suffix.starts_with('u');
        const std::uint64_t magnitude =
            signed_type ? static_cast<std::uint64_t>(std::abs(parsed))
                        : static_cast<std::uint64_t>(parsed);
        const bool fits = [&]() {
            if (!signed_type) {
                if (bits == 64U)
                    return true;
                return magnitude <= (std::uint64_t{1} << bits) - 1U;
            }
            if (bits == 64U)
                return true;
            const std::int64_t max = static_cast<std::int64_t>(std::uint64_t{1} << (bits - 1U)) - 1;
            return parsed >= -max - 1 && parsed <= max;
        }();
        if (!fits)
            return false;
        out.intValue = parsed;
        out.isSigned = signed_type;
        out.bits     = static_cast<std::uint8_t>(bits);
        return true;
    }

    // A plain integer (unsuffixed is i32 in Zith).  `support::looksIntegerLiteral`
    // accepts sign prefixes too, so the tokenizer keeps `-42` together.
    if (support::parseIntegerLiteral(token, out.intValue) == support::IntLiteralStatus::Overflow)
        return false;
    if (support::parseIntegerLiteral(token, out.intValue) == support::IntLiteralStatus::Ok) {
        if (out.intValue < std::numeric_limits<std::int32_t>::min() ||
            out.intValue > std::numeric_limits<std::int32_t>::max())
            return false;
        out.kind  = ConstantKind::Integer;
        out.bits  = 32;
        out.isSigned = true;
        return true;
    }

    // Unsuffixed float is f64 in Zith; `f`/`F` are f32.
    if (token.size() >= 2U && (token.back() == 'f' || token.back() == 'F')) {
        const std::string_view digits = token.substr(0U, token.size() - 1U);
        const std::string text        = std::string(digits);
        char *end                     = nullptr;
        out.floatValue                = std::strtod(text.c_str(), &end);
        if (end == nullptr || *end != '\0')
            return false;
        out.kind  = ConstantKind::Float;
        out.bits  = 32;
        out.isSigned = true;
        return true;
    }
    const std::string text = std::string(token);
    char *end      = nullptr;
    out.floatValue = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0')
        return false;
    out.kind      = ConstantKind::Float;
    out.bits      = 64;
    out.isSigned  = true;
    return true;
}

bool acceptConstant(const CXCursor cursor, const CXTranslationUnit unit, Constant &constant,
                    std::string &reason) {
    constant.name = takeString(clang_getCursorSpelling(cursor));
    if (constant.name.empty()) {
        reason = "empty macro name";
        return false;
    }

    std::vector<std::string> tokens;
    CXToken *raw_tokens  = nullptr;
    unsigned token_count = 0;
    const CXSourceRange extent = clang_getCursorExtent(cursor);
    clang_tokenize(unit, extent, &raw_tokens, &token_count);

    CXFile cursor_file     = nullptr;
    unsigned cursor_line   = 0;
    unsigned cursor_column = 0;
    clang_getSpellingLocation(clang_getCursorLocation(cursor), &cursor_file, &cursor_line,
                              &cursor_column, nullptr);

    for (unsigned index = 0; index < token_count; ++index) {
        CXFile file     = nullptr;
        unsigned line   = 0;
        unsigned column = 0;
        clang_getSpellingLocation(clang_getTokenLocation(unit, raw_tokens[index]), &file, &line,
                                  &column, nullptr);
        // Macro replacement tokens are on the same physical line.  This also
        // drops the terminating newline token that some libclang builds emit.
        if (file == nullptr || line != cursor_line)
            continue;
        if (column <= cursor_column)
            continue;
        const auto spelling = takeString(clang_getTokenSpelling(unit, raw_tokens[index]));
        if (!spelling.empty())
            tokens.push_back(std::move(spelling));
    }
    clang_disposeTokens(unit, raw_tokens, token_count);

    // The C tokenizer keeps unary `-` separate from a numeric literal.  Joining
    // the two-token shape lets `#define NEG -7i64` import as one integer.
    if (tokens.size() == 2U && tokens[0] == "-") {
        if (tokens[1].empty() || tokens[1].front() == '\'' || tokens[1].front() == '"')
            tokens.clear();
        else {
            tokens[0] = "-" + tokens[1];
            tokens.resize(1U);
        }
    }
    if (tokens.size() != 1U) {
        reason = "replacement is not a single scalar token";
        return false;
    }

    Scalars scalar;
    if (!scalarFromTokens(tokens.front(), scalar)) {
        reason = "unsupported replacement literal '" + tokens.front() + "'";
        return false;
    }
    constant.kind         = scalar.kind;
    constant.bits         = scalar.bits;
    constant.isSigned     = scalar.isSigned;
    constant.integerValue = scalar.intValue;
    constant.floatValue   = scalar.floatValue;
    constant.boolValue    = scalar.boolValue;
    constant.charValue    = scalar.charValue;
    return true;
}

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
    if (clang_getCursorKind(cursor) == CXCursor_MacroDefinition &&
        clang_Cursor_isMacroFunctionLike(cursor) != 0 && isMainFile(cursor, state.mainFile)) {
        state.artifact.skippedFunctions.push_back(
            takeString(clang_getCursorSpelling(cursor)) + ": macro skipped: function-like");
        return CXChildVisit_Continue;
    }
    if (clang_getCursorKind(cursor) == CXCursor_MacroDefinition &&
        isMainFile(cursor, state.mainFile) &&
        clang_Cursor_isMacroFunctionLike(cursor) == 0 &&
        clang_Cursor_isMacroBuiltin(cursor) == 0) {
        Constant constant;
        std::string reason;
        const auto name = takeString(clang_getCursorSpelling(cursor));
        constant.name   = name;
        if (isCompilerPredefined(name)) {
            state.artifact.skippedFunctions.push_back(name + ": macro skipped: compiler predefined");
        } else if (!acceptConstant(cursor, state.unit, constant, reason)) {
            state.artifact.skippedFunctions.push_back(
                constant.name.empty()
                    ? "macro: " + reason
                    : constant.name + ": macro skipped: " + reason);
        } else {
            bool duplicate = false;
            for (const auto &existing : state.artifact.constants) {
                if (existing.name == constant.name) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                state.artifact.skippedFunctions.push_back(constant.name +
                                                          ": duplicate constant declaration");
            } else {
                state.artifact.constants.push_back(std::move(constant));
            }
        }
        return CXChildVisit_Continue;
    }
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

[[maybe_unused]] std::vector<std::string> probeSystemIncludeDirs(const std::string &targetTriple,
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

[[maybe_unused]] std::vector<std::string> fallbackSystemIncludeDirs(const std::string &sysroot) {
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
    ParseState state{main_file, unit, *artifact};
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
    static auto &cacheMutex = *new std::mutex();

    const std::string key = targetTriple + '\0' + sysroot;

    // Intentionally leaked: the cache lives for the whole process and must not be
    // torn down at exit while other translation units may still query it.
    static auto &cache = *new std::map<std::string, std::vector<std::string>>();

    const std::lock_guard<std::mutex> lock(cacheMutex);
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
