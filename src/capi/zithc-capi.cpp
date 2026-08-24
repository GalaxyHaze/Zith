#include "zithc-capi.h"
#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/diagnostic.hpp"
#include "diagnostics/error-codes.hpp"
#include "sema/heuristic-engine.hpp"
#include "session/compilation-session.hpp"

#include "frontend/frontend.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"

#include <cstring>
#include <new>
#include <string>

struct zithc_session {
    zith::memory::Arena arena;
    zith::memory::StringInterner pool;
    zith::Options opts;
    zith::session::CompilationSession session;
    std::string last_error;
    std::string hover_result_;
    std::string output_result_;

    zithc_session(const char *file_path)
        : arena(), pool(arena), opts(arena), session(opts, file_path) {
        session.setBuffered(true);
    }
    zithc_session(const char *uri, const char *content, size_t length)
        : arena(), pool(arena), opts(arena), session(opts, uri) {
        session.setBuffered(true);
        session.setContent(std::string(content, length));
    }
};

static_assert(static_cast<int>(zith::diagnostics::Severity::Note) == ZITHC_SEVERITY_NOTE,
              "severity enum mismatch");
static_assert(static_cast<int>(zith::session::Stage::Source) == ZITHC_STAGE_SOURCE,
              "stage enum mismatch");
static_assert(static_cast<int>(zith::session::Stage::NraResolved) == ZITHC_STAGE_NRA_RESOLVED,
              "stage enum mismatch");
static_assert(static_cast<int>(zith::session::Stage::CodegenReady) == ZITHC_STAGE_CODEGEN_READY,
              "stage enum mismatch");
static_assert(static_cast<int>(zith::session::Stage::Cached) == ZITHC_STAGE_CACHED,
              "stage enum mismatch");

namespace {

/// Render a modern frontend TypeExpression as a short string.
std::string renderTypeExpr(const zith::frontend::TypeExpression &te,
                           const std::vector<zith::frontend::TypeExpression> &all_types) {
    switch (te.kind) {
    case zith::frontend::TypeExprKind::Error:
        return "?";
    case zith::frontend::TypeExprKind::Name:
        return te.name.empty() ? "?" : te.name;
    case zith::frontend::TypeExprKind::Pointer:
        return "*" + (te.arguments.empty()
                          ? "?"
                          : renderTypeExpr(all_types[te.arguments[0].value - 1U], all_types));
    case zith::frontend::TypeExprKind::Optional:
        return "?" + (te.arguments.empty()
                          ? "?"
                          : renderTypeExpr(all_types[te.arguments[0].value - 1U], all_types));
    case zith::frontend::TypeExprKind::Array: {
        std::string inner = te.arguments.empty()
                                ? "?"
                                : renderTypeExpr(all_types[te.arguments[0].value - 1U], all_types);
        return "[" + std::to_string(te.arrayLength) + "]" + inner;
    }
    case zith::frontend::TypeExprKind::Function: {
        std::string result = "fn(";
        for (size_t i = 0; i + 1U < te.arguments.size(); ++i) {
            if (i > 0)
                result += ", ";
            result += renderTypeExpr(all_types[te.arguments[i].value - 1U], all_types);
        }
        result += "): ";
        if (te.arguments.empty())
            result += "?";
        else
            result += renderTypeExpr(all_types[te.arguments.back().value - 1U], all_types);
        return result;
    }
    case zith::frontend::TypeExprKind::Opaque:
        return "raw opaque";
    case zith::frontend::TypeExprKind::Slice: {
        std::string inner = te.arguments.empty()
                                ? "?"
                                : renderTypeExpr(all_types[te.arguments[0].value - 1U], all_types);
        return "[]" + inner;
    }
    }
    return "?";
}

/// Find the frontend declaration matching a symbol-table entry via snapshot.
const zith::frontend::Declaration *
findFrontendDecl(const zith::symbols::SymbolData &data,
                 const std::shared_ptr<const zith::session::CompilationSnapshot> &snapshot,
                 const zith::memory::StringInterner &interner) {
    if (!snapshot)
        return nullptr;
    const auto name = interner.lookup(data.name);
    for (const auto &module : snapshot->modules()) {
        if (!module->frontend)
            continue;
        for (const auto &decl : module->frontend->declarations()) {
            if (decl.kind == zith::frontend::DeclKind::Function && decl.name == name)
                return &decl;
        }
    }
    return nullptr;
}

} // anonymous namespace

extern "C" {

zithc_session *zithc_session_create(const char *file_path) {
    if (!file_path || file_path[0] == '\0')
        return nullptr;
    auto *s = new (std::nothrow) zithc_session(file_path);
    return s;
}

zithc_session *zithc_session_create_from_buffer(const char *uri, const char *content,
                                                size_t length) {
    if (!uri || uri[0] == '\0' || !content)
        return nullptr;
    auto *s = new (std::nothrow) zithc_session(uri, content, length);
    return s;
}

void zithc_session_destroy(zithc_session *session) {
    delete session;
}

void zithc_session_add_include_dir(zithc_session *session, const char *dir) {
    if (session && dir)
        session->opts.includeDirs.push(std::string(dir));
}

void zithc_session_set_opt_level(zithc_session *session, uint8_t level) {
    if (session)
        session->opts.flags.optLevel(level);
}

void zithc_session_set_mode(zithc_session *session, uint8_t mode) {
    if (session)
        session->opts.flags.mode(static_cast<zith::Options::Mode>(mode));
}

void zithc_session_set_emit_tokens(zithc_session *session, bool tokens) {
    if (session)
        session->opts.flags.emitTokens(tokens);
}

void zithc_session_set_emit_flags(zithc_session *session, bool ast, bool hir, bool ir, bool asm_) {
    if (session) {
        session->opts.flags.emitAst(ast);
        session->opts.flags.emitHir(hir);
        session->opts.flags.emitIr(ir);
        session->opts.flags.emitAsm(asm_);
    }
}

void zithc_session_set_target(zithc_session *session, int stage) {
    if (session)
        session->opts.targetStage = static_cast<zith::session::Stage>(stage);
}

bool zithc_run(zithc_session *session) {
    if (!session)
        return false;
    return session->session.run();
}

bool zithc_run_to(zithc_session *session, int stage) {
    if (!session)
        return false;
    if (stage < ZITHC_STAGE_SOURCE || stage > ZITHC_STAGE_CACHED)
        return false;
    auto s = static_cast<zith::session::Stage>(stage);
    return session->session.runTo(s);
}

size_t zithc_diag_count(zithc_session *session) {
    if (!session)
        return 0;
    return session->session.diags().all().size();
}

zithc_diagnostic zithc_diag_get(zithc_session *session, size_t index) {
    zithc_diagnostic result = {ZITHC_SEVERITY_ERROR, 0, nullptr, {0, 0}};
    if (!session)
        return result;
    auto &all = session->session.diags().diagnostics();
    if (index >= all.size())
        return result;
    auto &d = all[index];
    // Auto-fill suggestions for UndefinedIdent diagnostics
    if (d.code == zith::diagnostics::err::UndefinedIdent && d.suggestions.empty()) {
        zith::sema::HeuristicEngine heuristic;
        heuristic.generate(d, session->session.symbolTable(), d.suggestions);
    }
    // Enrich overload errors with function signatures from the modern frontend
    if ((d.code == zith::diagnostics::err::NoMatchingFn ||
         d.code == zith::diagnostics::err::AmbiguousCall) &&
        d.suggestions.empty()) {
        auto start = d.message.find('\'');
        auto end   = d.message.rfind('\'');
        if (start != std::string::npos && end != std::string::npos && end > start) {
            auto fn_name = d.message.substr(start + 1, end - start - 1);
            zith::memory::Arena tmp_arena;
            auto syms = session->session.symbolTable().lookupAll(fn_name, tmp_arena);
            for (auto sym_id : syms) {
                auto &data = session->session.symbolTable().get(sym_id);
                if (data.kind != zith::symbols::SymKind::Fn)
                    continue;
                const auto *decl = findFrontendDecl(data, session->session.snapshot(),
                                                    session->session.symbolTable().interner());
                if (!decl)
                    continue;
                std::string sig       = "   fn " + decl->name + "(";
                const auto &all_types = session->session.snapshot()
                                            ? session->session.snapshot()
                                                  ->modules()
                                                  .front()
                                                  ->frontend->typeExpressions()
                                            : std::vector<zith::frontend::TypeExpression>{};
                for (size_t i = 0; i < decl->parameters.size(); ++i) {
                    if (i > 0)
                        sig += ", ";
                    sig += decl->parameters[i].name + ": ";
                    if (decl->parameters[i].type) {
                        auto type_id = decl->parameters[i].type.value - 1U;
                        if (type_id < all_types.size())
                            sig += renderTypeExpr(all_types[type_id], all_types);
                        else
                            sig += "?";
                    }
                }
                sig += ")";
                if (decl->declaredType) {
                    auto type_id = decl->declaredType.value - 1U;
                    if (type_id < all_types.size())
                        sig += " -> " + renderTypeExpr(all_types[type_id], all_types);
                }
                d.suggestions.push(std::move(sig));
            }
        }
    }
    result.severity = static_cast<zithc_severity>(static_cast<int>(d.severity));
    result.code     = d.code;
    result.message  = d.message.c_str();
    result.span     = {d.primary.start, d.primary.end};
    return result;
}
size_t zithc_diag_suggestion_count(zithc_session *session, size_t diag_index) {
    if (!session)
        return 0;
    auto &all = session->session.diags().diagnostics();
    if (diag_index >= all.size())
        return 0;
    return all[diag_index].suggestions.size();
}

const char *zithc_diag_suggestion_get(zithc_session *session, size_t diag_index, size_t sug_index) {
    if (!session)
        return nullptr;
    auto &all = session->session.diags().diagnostics();
    if (diag_index >= all.size())
        return nullptr;
    auto &sug = all[diag_index].suggestions;
    if (sug_index >= sug.size())
        return nullptr;
    return sug[sug_index].c_str();
}

bool zithc_has_errors(zithc_session *session) {
    return session && session->session.hasErrors();
}

void zithc_emit_diagnostics(zithc_session *session) {
    if (session)
        session->session.emitDiagnostics();
}

zithc_position zithc_offset_to_position(zithc_session *session, uint32_t offset) {
    zithc_position result = {0, 0};
    if (!session)
        return result;
    auto &s = session->session;
    zith::memory::Span span{s.fileId(), offset, offset + 1};
    auto loc    = s.sourceMap().loc(span);
    result.line = loc.line > 0 ? loc.line - 1 : 0;
    result.col  = loc.col > 0 ? loc.col - 1 : 0;
    return result;
}

const char *zithc_hover(zithc_session *session, uint32_t offset) {
    if (!session)
        return nullptr;
    const auto snapshot = session->session.snapshot();
    if (!snapshot)
        return nullptr;
    // Find the root module's frontend snapshot
    const auto *root_mod = snapshot->findModule(
        zith::session::SourceCatalog::canonicalPath(session->session.filePath()));
    if (!root_mod || !root_mod->frontend)
        return nullptr;

    const auto &tokens = root_mod->frontend->tokens();
    const auto &source = root_mod->frontend->source();
    std::string ident_name;
    for (const auto &tok : tokens) {
        if (tok.span.start <= offset && offset < tok.span.end) {
            if (tok.kind == zith::frontend::TokenKind::Identifier)
                ident_name.assign(source, tok.span.start, tok.span.end - tok.span.start);
            break;
        }
    }
    if (ident_name.empty())
        return nullptr;

    zith::memory::Arena tmp_arena;
    auto all = session->session.symbolTable().lookupAll(ident_name, tmp_arena);
    std::string result;
    for (auto sym_id : all) {
        auto &data = session->session.symbolTable().get(sym_id);
        if (data.kind != zith::symbols::SymKind::Fn)
            continue;
        const auto *decl =
            findFrontendDecl(data, snapshot, session->session.symbolTable().interner());
        if (!decl)
            continue;
        const auto &all_types = root_mod->frontend->typeExpressions();
        if (!result.empty())
            result += "\n---\n";
        result += "**fn** `" + decl->name + "(";
        for (size_t i = 0; i < decl->parameters.size(); ++i) {
            if (i > 0)
                result += ", ";
            result += decl->parameters[i].name + ": ";
            if (decl->parameters[i].type) {
                auto type_id = decl->parameters[i].type.value - 1U;
                if (type_id < all_types.size())
                    result += "`" + renderTypeExpr(all_types[type_id], all_types) + "`";
            }
        }
        result += ")";
        if (decl->declaredType) {
            auto type_id = decl->declaredType.value - 1U;
            if (type_id < all_types.size())
                result += " -> `" + renderTypeExpr(all_types[type_id], all_types) + "`";
        }
    }
    if (result.empty())
        return nullptr;
    result                 = "```zith\n" + result + "\n```";
    session->hover_result_ = std::move(result);
    return session->hover_result_.c_str();
}

const char *zithc_last_error(zithc_session *session) {
    if (!session)
        return "null session";
    return session->last_error.c_str();
}

const char *zithc_run_to_json(zithc_session *session, int stage) {
    if (!session)
        return R"({"success":false,"errors":[{"severity":"error","code":0,"message":"null session","span":{"start":0,"end":0}}]})";

    if (stage < 0)
        session->session.run();
    else
        session->session.runTo(static_cast<zith::session::Stage>(stage));

    auto &diags = session->session.diags();
    auto &all   = diags.diagnostics();

    std::string json;
    json += "{\"success\":";
    json += diags.hasErrors() ? "false" : "true";
    json += ",\"errors\":[";

    for (size_t i = 0; i < all.size(); ++i) {
        if (i > 0)
            json += ",";
        auto &d = all[i];
        json += "{\"severity\":\"";
        switch (d.severity) {
        case zith::diagnostics::Severity::Note:
            json += "note";
            break;
        case zith::diagnostics::Severity::Warning:
            json += "warning";
            break;
        case zith::diagnostics::Severity::Error:
            json += "error";
            break;
        case zith::diagnostics::Severity::Bug:
            json += "bug";
            break;
        }
        json += "\",\"code\":";
        json += std::to_string(d.code);
        json += ",\"message\":\"";
        // Escape JSON special chars in message
        for (auto c : d.message) {
            switch (c) {
            case '"':
                json += "\\\"";
                break;
            case '\\':
                json += "\\\\";
                break;
            case '\n':
                json += "\\n";
                break;
            case '\r':
                json += "\\r";
                break;
            case '\t':
                json += "\\t";
                break;
            default:
                json += c;
            }
        }
        json += "\",\"span\":{\"start\":";
        json += std::to_string(d.primary.start);
        json += ",\"end\":";
        json += std::to_string(d.primary.end);
        json += "}}";
    }

    json += "]}";
    session->last_error = std::move(json);
    return session->last_error.c_str();
}

const char *zithc_session_flush_output(zithc_session *session) {
    if (!session)
        return "";
    session->output_result_ = session->session.flushOutput();
    return session->output_result_.c_str();
}
} // extern "C"
