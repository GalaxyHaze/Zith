#include "frontend/lexer/lexer.hpp"
#include "frontend/parser/parse.hpp"
#include "resolution/resolution.hpp"
#include "session/dispatch.hpp"
#include "session/session.hpp"
#include "sema/sema.hpp"

namespace toolkit::session {

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

    return generated_lexer::tokenize(loc->get().slice());
}

template <>
common::memory::Result<ParsedResult> dispatch<Stage::Parsed>(CompilationSession &session) {
    auto &context = session.context();
    if (!session.hasStageResult<Stage::Lexed>())
        return common::memory::Error{"Parsed: missing Lexed result"};
    if (!context.sourceMap.exists(context.fileId))
        return common::memory::Error{"Parsed: missing source"};

    const auto loc = context.sourceMap.get(context.fileId);
    if (!loc)
        return common::memory::Error{"Parsed: missing source"};

    auto tokens = generated_lexer::tokenize(loc->get().slice());

    generated_parser::Parser<sample::ParseOutput> parser(context.arena);
    sample::ParseOutput output =
        hooks::parser::parseSource(parser, tokens, loc->get().slice());

    for (const auto &diagnostic : output.diagnostics) {
        session.diags().push(Diagnostic{
            .span = common::memory::SourceSpan{context.fileId, diagnostic.span},
            .message = diagnostic.message,
        });
    }
    return common::memory::Result<ParsedResult>{std::move(output)};
}

template <>
common::memory::Result<SourceResult> dispatch<Stage::Source>(CompilationSession &) {
    return {};
}

template <>
common::memory::Result<ScannedResult> dispatch<Stage::Scanned>(CompilationSession &session) {
    auto &context = session.context();
    if (!session.hasStageResult<Stage::Parsed>())
        return common::memory::Error{"Scanned: missing Parsed result"};
    auto &parsed = session.stageResult<Stage::Parsed>().value();
    if (parsed.ast.root == nullptr)
        return common::memory::Error{"Scanned: parsed program is missing"};
    context.scan.records.clear();
    auto result = toolkit::resolution::scanProgram(*parsed.ast.root, context.fileId, context.scan);
    if (result.isError())
        return result.error();
    return {};
}

template <>
common::memory::Result<ImportedResult> dispatch<Stage::Imported>(CompilationSession &session) {
    auto &context = session.context();
    if (!session.hasStageResult<Stage::Parsed>())
        return common::memory::Error{"Imported: missing Parsed result"};
    auto &parsed = session.stageResult<Stage::Parsed>().value();
    auto result = toolkit::resolution::importProgram(
        parsed, context.fileId, context.projectRoot, context.sourceMap, context.imports);
    if (result.isError())
        return result.error();
    for (const auto &diag : context.imports.diagnosticSink()) {
        session.diags().push(diag);
    }
    return {};
}

template <>
common::memory::Result<ResolvedResult> dispatch<Stage::Resolved>(CompilationSession &session) {
    auto &context = session.context();
    if (!session.hasStageResult<Stage::Imported>())
        return common::memory::Error{"Resolved: missing Imported result"};
    auto result =
        toolkit::resolution::resolveModules(context.imports, context.resolved);
    if (result.isError())
        return result.error();
    return {};
}

template <>
common::memory::Result<TypeCheckedResult>
dispatch<Stage::TypeChecked>(CompilationSession &session) {
    if (!session.hasStageResult<Stage::Parsed>())
        return common::memory::Error{"TypeChecked: missing Parsed result"};
    if (!session.context().sourceMap.exists(session.context().fileId))
        return common::memory::Error{"TypeChecked: missing source"};

    auto &context = session.context();
    auto &parsed = session.stageResult<Stage::Parsed>().value();
    const bool ok = toolkit::sema::typeCheckProgram(
        *parsed.ast.root, context.fileId, context.arena, context.interner,
        session.diags(), context.checked);
    if (!ok)
        return common::memory::Error{"TypeChecked: semantic errors"};
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

} // namespace toolkit::session
