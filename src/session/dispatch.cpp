#include "frontend/lexer/lexer.hpp"
#include "session/dispatch.hpp"
#include "session/session.hpp"

namespace zith::session {

template <>
memory::Result<LexedResult> dispatch<Stage::Lexed>(CompilationSession &session) {
    auto &context = session.context();

    if (context.filePath.empty())
        return memory::Error{"Lexed: missing file path"};

    if (!context.sourceMap.exists(context.fileId)) {
        const auto loaded = context.sourceMap.loadFile(context.filePath);
        if (!loaded) {
            session.diags().push(Diagnostic{
                .span = {.file = 0, .start = 0, .end = 0},
                .message = loaded.error().msg,
            });
            return memory::Error{loaded.error().msg};
        }
        context.fileId = loaded.value();
    }

    const auto loc = context.sourceMap.get(context.fileId);
    if (!loc) {
        session.diags().push(Diagnostic{
            .span = {.file = 0, .start = 0, .end = 0},
            .message = "Source file disappeared from session",
        });
        return memory::Error{"Source file disappeared from session"};
    }

    return generated_lexer::tokenize(loc->get().slice(), context.interner);
}

template <>
memory::Result<SourceResult> dispatch<Stage::Source>(CompilationSession &) {
    return {};
}

template <>
memory::Result<ScannedResult> dispatch<Stage::Scanned>(CompilationSession &) {
    return {};
}

template <>
memory::Result<ImportedResult> dispatch<Stage::Imported>(CompilationSession &) {
    return {};
}

template <>
memory::Result<ResolvedResult> dispatch<Stage::Resolved>(CompilationSession &) {
    return {};
}

template <>
memory::Result<TypeCheckedResult> dispatch<Stage::TypeChecked>(CompilationSession &) {
    return {};
}

template <>
memory::Result<SolvedResult> dispatch<Stage::Solved>(CompilationSession &) {
    return {};
}

template <>
memory::Result<NraResolvedResult> dispatch<Stage::NraResolved>(CompilationSession &) {
    return {};
}

template <>
memory::Result<HirLoweredResult> dispatch<Stage::HirLowered>(CompilationSession &) {
    return {};
}

template <>
memory::Result<CodegenReadyResult> dispatch<Stage::CodegenReady>(CompilationSession &) {
    return {};
}

template <>
memory::Result<CachedResult> dispatch<Stage::Cached>(CompilationSession &) {
    return {};
}

} // namespace zith::session
