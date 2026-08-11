#include "frontend/lexer/lexer.hpp"
#include "session/dispatch.hpp"
#include "session/session.hpp"

namespace zith::session {

template <>
common::memory::Result<LexedResult> dispatch<Stage::Lexed>(CompilationSession &session) {
    auto &context = session.context();

    if (context.filePath.empty())
        return common::memory::Error{"Lexed: missing file path"};

    if (!context.sourceMap.exists(context.fileId)) {
        const auto loaded = context.sourceMap.loadFile(context.filePath);
        if (!loaded) {
            session.diags().push(Diagnostic{
                .span = common::memory::SourceSpan{context.fileId, common::memory::Span{0, 0}},
                .message = loaded.error().msg,
            });
            return common::memory::Error{loaded.error().msg};
        }
        context.fileId = loaded.value();
    }

    const auto loc = context.sourceMap.get(context.fileId);
    if (!loc) {
        session.diags().push(Diagnostic{
            .span = common::memory::SourceSpan{context.fileId, common::memory::Span{0, 0}},
            .message = "Source file disappeared from session",
        });
        return common::memory::Error{"Source file disappeared from session"};
    }

    return generated_lexer::tokenize(loc->get().slice(), context.interner);
}

template <>
common::memory::Result<SourceResult> dispatch<Stage::Source>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<ScannedResult> dispatch<Stage::Scanned>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<ImportedResult> dispatch<Stage::Imported>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<ResolvedResult> dispatch<Stage::Resolved>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<TypeCheckedResult> dispatch<Stage::TypeChecked>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<SolvedResult> dispatch<Stage::Solved>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<NraResolvedResult> dispatch<Stage::NraResolved>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<HirLoweredResult> dispatch<Stage::HirLowered>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<CodegenReadyResult> dispatch<Stage::CodegenReady>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<CachedResult> dispatch<Stage::Cached>(CompilationSession &) {
    return {};
}

} // namespace zith::session
